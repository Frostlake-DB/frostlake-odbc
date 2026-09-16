/*
 * Copyright 2026 MLorek
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The Frostlake wire protocol, as one function: POST a SqlRequest to
 * /api/execute, decode the SqlResponse. The server answers 200 for BOTH
 * success and SQL failure (the latter as success=false + errorMessage), so a
 * non-200 status is strictly a transport-level problem.
 */

#include "fl_odbc.h"
#include "http.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *build_request_json(const fl_dbc *dbc, const char *sql, SQLLEN multi_statement_count) {
    fl_strbuf buf;
    fl_strbuf_init(&buf);
    int failed = fl_strbuf_append(&buf, "{\"sql\":");
    failed = failed || fl_strbuf_append_json_string(&buf, sql);
    if (dbc->session_id != NULL) {
        failed = failed || fl_strbuf_append(&buf, ",\"sessionId\":");
        failed = failed || fl_strbuf_append_json_string(&buf, dbc->session_id);
    }
    if (multi_statement_count >= 0) {
        /* Absent unless the statement declared one, so a request that says nothing leaves the
         * session's MULTI_STATEMENT_COUNT in charge. */
        char declared[48];
        snprintf(declared, sizeof(declared), ",\"multiStatementCount\":%ld", (long) multi_statement_count);
        failed = failed || fl_strbuf_append(&buf, declared);
    }
    failed = failed || fl_strbuf_append(&buf, ",\"autoCommit\":true}");
    if (failed) {
        fl_strbuf_free(&buf);
        return NULL;
    }
    return buf.data;
}

static int decode_columns(const fl_json *columns_json, fl_resultset *set) {
    int count = columns_json != NULL ? (int) columns_json->child_count : 0;
    set->column_count = count;
    set->columns = calloc(count > 0 ? (size_t) count : 1, sizeof(fl_column));
    if (set->columns == NULL) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        const fl_json *column_json = fl_json_at(columns_json, (size_t) i);
        fl_column *column = &set->columns[i];
        column->name = fl_strdup(fl_json_get_string(column_json, "name"));
        column->type_name = fl_strdup(fl_json_get_string(column_json, "dataType"));
        column->precision = (SQLINTEGER) fl_json_get_long(column_json, "precision", 0);
        column->scale = (SQLINTEGER) fl_json_get_long(column_json, "scale", 0);
        /* Absent for every type but text and binary, and on any engine predating the field. */
        column->length = (SQLINTEGER) fl_json_get_long(column_json, "length", 0);
        column->sql_type = fl_sql_type_for(column->type_name, column->scale);
        if (column->name == NULL) {
            column->name = fl_strdup("");
        }
    }
    return 0;
}

static int decode_rows(const fl_json *rows_json, fl_resultset *set) {
    int rows = rows_json != NULL ? (int) rows_json->child_count : 0;
    set->row_count = rows;
    size_t cell_count = (size_t) rows * (size_t) set->column_count;
    set->cells = calloc(cell_count > 0 ? cell_count : 1, sizeof(fl_cell));
    if (set->cells == NULL) {
        return -1;
    }
    for (int r = 0; r < rows; r++) {
        const fl_json *row_json = fl_json_at(rows_json, (size_t) r);
        for (int c = 0; c < set->column_count; c++) {
            fl_cell *cell = &set->cells[r * set->column_count + c];
            const fl_json *value = fl_json_at(row_json, (size_t) c);
            if (value == NULL || value->kind == FL_JSON_NULL) {
                cell->is_null = 1;
                cell->kind = 's';
                continue;
            }
            switch (value->kind) {
                case FL_JSON_NUMBER:
                    cell->kind = 'n';
                    cell->text = fl_strdup(value->text);
                    break;
                case FL_JSON_BOOL:
                    /* ODBC surfaces BOOLEAN as SQL_BIT, whose character form is 1/0. That spelling
                     * belongs to the column type, not to the JSON shape: the engine also sends a
                     * boolean in a column it declares VARCHAR (SYSTEM$STREAM_HAS_DATA, a variable
                     * holding one), and there a character read has to say true/false. */
                    cell->kind = 'b';
                    cell->text = set->columns[c].sql_type == SQL_BIT
                        ? fl_strdup(strcmp(value->text, "true") == 0 ? "1" : "0")
                        : fl_strdup(value->text);
                    break;
                default:
                    cell->kind = 's';
                    cell->text = fl_strdup(value->text != NULL ? value->text : "");
            }
            if (cell->text == NULL) {
                return -1;
            }
        }
    }
    return 0;
}

fl_response *fl_proto_execute(fl_dbc *dbc, const char *sql, char **transport_error) {
    return fl_proto_execute_counted(dbc, sql, -1, transport_error);
}

fl_response *fl_proto_execute_counted(fl_dbc *dbc, const char *sql, SQLLEN multi_statement_count,
                                      char **transport_error) {
    *transport_error = NULL;

    char *request = build_request_json(dbc, sql, multi_statement_count);
    if (request == NULL) {
        *transport_error = fl_strdup("out of memory building request");
        return NULL;
    }

    int status = 0;
    char *body = NULL;
    int rc = fl_http_post(dbc->host, dbc->port, "/api/execute", request,
                          dbc->request_timeout, &status, &body, transport_error);
    free(request);
    if (rc != 0) {
        return NULL;
    }

    fl_json *root = fl_json_parse(body, strlen(body));
    if (root == NULL) {
        char *message = malloc(strlen(body) + 64);
        if (message != NULL) {
            snprintf(message, strlen(body) + 64, "unparseable response (HTTP %d): %.200s", status, body);
        }
        free(body);
        *transport_error = message != NULL ? message : fl_strdup("unparseable response");
        return NULL;
    }
    free(body);

    fl_response *response = calloc(1, sizeof(fl_response));
    if (response == NULL) {
        fl_json_free(root);
        *transport_error = fl_strdup("out of memory");
        return NULL;
    }

    response->session_id = fl_strdup(fl_json_get_string(root, "sessionId"));
    int success = fl_json_get_bool(root, "success", 0);
    if (!success) {
        const char *error_message = fl_json_get_string(root, "errorMessage");
        if (error_message == NULL) {
            /* transport-shaped error bodies use {"error": "..."} */
            error_message = fl_json_get_string(root, "error");
        }
        response->error_message = fl_strdup(error_message != NULL ? error_message : "unknown server error");
    } else {
        const fl_json *sets_json = fl_json_get(root, "resultSets");
        int set_count = sets_json != NULL ? (int) sets_json->child_count : 0;
        response->sets = calloc(set_count > 0 ? (size_t) set_count : 1, sizeof(fl_resultset));
        if (response->sets == NULL) {
            fl_json_free(root);
            fl_response_free(response);
            *transport_error = fl_strdup("out of memory");
            return NULL;
        }
        response->set_count = set_count;
        for (int i = 0; i < set_count; i++) {
            const fl_json *set_json = fl_json_at(sets_json, (size_t) i);
            if (decode_columns(fl_json_get(set_json, "columns"), &response->sets[i]) != 0
                || decode_rows(fl_json_get(set_json, "rows"), &response->sets[i]) != 0) {
                fl_json_free(root);
                fl_response_free(response);
                *transport_error = fl_strdup("out of memory decoding result set");
                return NULL;
            }
        }
    }
    fl_json_free(root);

    /* pin the server-assigned session for every later statement */
    if (response->session_id != NULL) {
        free(dbc->session_id);
        dbc->session_id = fl_strdup(response->session_id);
    }
    return response;
}
