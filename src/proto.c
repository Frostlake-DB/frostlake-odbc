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
 * non-200 status is strictly a transport-level problem — with one exception: a
 * request that asked to resume its session (requireSession) and named one the
 * engine no longer holds is answered 404, and nothing ran. That one is handled
 * here, below every caller: see fl_proto_execute_as.
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
        /* Resume this session or refuse: without it, an engine whose session has gone runs the
         * statement in a fresh one under the same id, at its default scope. Only an engine known
         * to understand the field is sent it; an older one's parser may refuse a field it never
         * knew. */
        if (dbc->tracks_sessions == 1) {
            failed = failed || fl_strbuf_append(&buf, ",\"requireSession\":true");
        }
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

/* ---- the session ----------------------------------------------------------- */

static const char LOST_TRANSACTION[] =
    "the engine no longer holds this connection's session (it expired, was released, or the "
    "server restarted), so its open transaction is gone; the statement did not run";
static const char LOST_CONTEXT[] =
    "the engine no longer holds this connection's session (it expired, was released, or the "
    "server restarted), and the context set up on it (USE, SET, ALTER SESSION or a temporary "
    "object) went with it, so the statement was not run again; the next statement starts a "
    "fresh session on the connection's scope";
static const char LOST_AGAIN[] =
    "the engine refused a session it had just started; the statement did not run";

static void replace_text(char **slot, const char *value) {
    free(*slot);
    *slot = value != NULL ? fl_strdup(value) : NULL;
}

/* Back to what a fresh session holds: none of the application's context, no transaction, and the
 * DSN's scope still to apply — which is also where the catalog the application sees goes back
 * to. */
static void reset_session_state(fl_dbc *dbc) {
    dbc->session_dirty = 0;
    dbc->in_transaction = 0;
    dbc->scope_pending = 1;
    replace_text(&dbc->database, dbc->scope_database);
    replace_text(&dbc->schema, dbc->scope_schema);
}

/* Forgets the session id and what the driver knew of the session behind it, so the next
 * statement starts a fresh one on the DSN's scope. */
static void forget_session(fl_dbc *dbc) {
    free(dbc->session_id);
    dbc->session_id = NULL;
    reset_session_state(dbc);
}

void fl_proto_session_reset(fl_dbc *dbc) {
    free(dbc->session_id);
    dbc->session_id = NULL;
    dbc->tracks_sessions = -1;
    dbc->session_dirty = 0;
    dbc->in_transaction = 0;
    dbc->scope_pending = 0;
}

/* Forgets a session the engine no longer holds. Answers -1 with *transport_error saying so when
 * the session held something a fresh one would not have — the statement is then reported, not
 * sent again — and 0 when it may go again on a fresh session. */
static int lose_session(fl_dbc *dbc, char **transport_error) {
    int had_transaction = dbc->in_transaction;
    int had_context = dbc->session_dirty;
    forget_session(dbc);
    if (had_transaction || had_context) {
        *transport_error = fl_strdup(had_transaction ? LOST_TRANSACTION : LOST_CONTEXT);
        return -1;
    }
    return 0;
}

/* Takes in what an answer says of the session: the id it ran in and, from the presence of
 * newSession, whether the engine tracks sessions at all. */
static void absorb_session(fl_dbc *dbc, const fl_json *root, int sent) {
    const char *session = fl_json_get_string(root, "sessionId");
    if (session == NULL || session[0] == '\0') {
        return;
    }
    replace_text(&dbc->session_id, session);
    const fl_json *started = fl_json_get(root, "newSession");
    if (started != NULL && started->kind == FL_JSON_BOOL) {
        dbc->tracks_sessions = 1;
        if (sent && strcmp(started->text, "true") == 0) {
            /* The engine ran the statement in a fresh session in place of ours: whatever the old
             * one held is gone, and the scope goes back on before the next statement. */
            reset_session_state(dbc);
        }
    } else if (dbc->tracks_sessions < 0) {
        dbc->tracks_sessions = 0;
    }
}

/* One POST /api/execute, without any recovery. Answers the decoded response; NULL with
 * *transport_error set on a transport failure; NULL with *gone set when the engine refused the
 * session id as one it does not hold — and then nothing ran. */
static fl_response *post(fl_dbc *dbc, const char *sql, SQLLEN multi_statement_count, int *gone,
                         char **transport_error) {
    *gone = 0;
    *transport_error = NULL;
    int sent = dbc->session_id != NULL;

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

    const char *named = fl_json_get_string(root, "sessionId");
    if (sent && status == 404 && !fl_json_get_bool(root, "success", 0)
        && (named == NULL || named[0] == '\0')) {
        fl_json_free(root);
        *gone = 1;
        return NULL;
    }

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

    /* pin the server-assigned session for every later statement */
    absorb_session(dbc, root, sent);
    fl_json_free(root);
    return response;
}

/* A response carrying only an error message, the way the engine answers a refused statement. */
static fl_response *refused(const char *prefix, const char *message) {
    fl_response *response = calloc(1, sizeof(fl_response));
    if (response == NULL) {
        return NULL;
    }
    size_t length = strlen(prefix) + strlen(message) + 1;
    response->error_message = malloc(length);
    if (response->error_message == NULL) {
        free(response);
        return NULL;
    }
    snprintf(response->error_message, length, "%s%s", prefix, message);
    return response;
}

/* Puts the connection's scope on the session, starting a fresh one when there is none: USE
 * DATABASE and USE SCHEMA as the DSN named them, and AUTOCOMMIT off again when the application
 * turned SQL_ATTR_AUTOCOMMIT off — a fresh session starts in autocommit mode, and statements the
 * application means to commit together would otherwise commit one by one. A session lost part way
 * takes whatever part of the scope was on with it, so the whole scope goes on again, once.
 *
 * Answers 0, or -1 with either *refusal holding the engine's answer to a statement of the scope it
 * refused, or *transport_error set. The scope stays pending after a failure, so the next statement
 * tries it again: a DSN naming a database that has gone keeps failing rather than letting later
 * statements run in the default scope. */
static int apply_scope(fl_dbc *dbc, fl_response **refusal, char **transport_error) {
    fl_strbuf scope[3];
    int count = 0;
    int failed = 0;
    if (dbc->scope_database != NULL && dbc->scope_database[0] != '\0') {
        fl_strbuf_init(&scope[count]);
        failed = failed || fl_strbuf_append(&scope[count], "USE DATABASE ") != 0
            || fl_strbuf_append_ident(&scope[count], dbc->scope_database) != 0;
        count++;
    }
    if (dbc->scope_schema != NULL && dbc->scope_schema[0] != '\0') {
        fl_strbuf_init(&scope[count]);
        failed = failed || fl_strbuf_append(&scope[count], "USE SCHEMA ") != 0
            || fl_strbuf_append_ident(&scope[count], dbc->scope_schema) != 0;
        count++;
    }
    if (!dbc->autocommit) {
        fl_strbuf_init(&scope[count]);
        failed = failed || fl_strbuf_append(&scope[count], "ALTER SESSION SET AUTOCOMMIT = FALSE") != 0;
        count++;
    }

    int outcome = 0;
    if (failed) {
        *transport_error = fl_strdup("out of memory building the connection's scope");
        outcome = -1;
    }
    int restarted = 0;
    for (int i = 0; outcome == 0 && i < count;) {
        int gone = 0;
        fl_response *response = post(dbc, scope[i].data, -1, &gone, transport_error);
        if (gone) {
            if (restarted) {
                forget_session(dbc);
                *transport_error = fl_strdup(LOST_AGAIN);
                outcome = -1;
            } else if (lose_session(dbc, transport_error) != 0) {
                outcome = -1;
            } else {
                restarted = 1;
                i = 0;
            }
            continue;
        }
        if (response == NULL) {
            outcome = -1;
        } else if (response->error_message != NULL) {
            *refusal = response;
            outcome = -1;
        } else {
            fl_response_free(response);
            i++;
        }
    }
    for (int i = 0; i < count; i++) {
        fl_strbuf_free(&scope[i]);
    }
    if (outcome == 0) {
        dbc->scope_pending = 0;
    }
    return outcome;
}

/* The answer for a statement whose scope could not be put on the session: the engine's refusal of
 * the scope, reported as the statement's, or the transport failure as it is. */
static fl_response *scope_failed(fl_response *refusal, char **transport_error) {
    if (refusal == NULL) {
        return NULL;
    }
    fl_response *response = refused("the connection's scope could not be applied: ",
                                     refusal->error_message);
    fl_response_free(refusal);
    if (response == NULL) {
        *transport_error = fl_strdup("out of memory");
    }
    return response;
}

fl_response *fl_proto_execute_as(fl_dbc *dbc, const char *sql, SQLLEN multi_statement_count,
                                 int flags, char **transport_error) {
    *transport_error = NULL;
    fl_response *refusal = NULL;
    if (dbc->scope_pending && apply_scope(dbc, &refusal, transport_error) != 0) {
        return scope_failed(refusal, transport_error);
    }

    int gone = 0;
    fl_response *response = post(dbc, sql, multi_statement_count, &gone, transport_error);
    if (gone) {
        /* The engine no longer holds this connection's session — it expired, was released, or
         * the server restarted — and nothing ran. With a transaction or a moved context gone
         * with it, running the statement again would put it somewhere its author did not
         * intend, so that is reported. Otherwise a fresh session on the scope takes over and the
         * statement is sent once more; a second refusal is reported rather than chased. */
        if (!(flags & FL_EXEC_RECOVER)) {
            forget_session(dbc);
            *transport_error = fl_strdup(LOST_AGAIN);
            return NULL;
        }
        if (lose_session(dbc, transport_error) != 0) {
            return NULL;
        }
        if (apply_scope(dbc, &refusal, transport_error) != 0) {
            return scope_failed(refusal, transport_error);
        }
        response = post(dbc, sql, multi_statement_count, &gone, transport_error);
        if (gone) {
            forget_session(dbc);
            *transport_error = fl_strdup(LOST_AGAIN);
            return NULL;
        }
    }
    if (response != NULL && response->error_message == NULL && (flags & FL_EXEC_TRACKED)) {
        fl_session_note(dbc, sql);
    }
    return response;
}

fl_response *fl_proto_execute(fl_dbc *dbc, const char *sql, char **transport_error) {
    return fl_proto_execute_counted(dbc, sql, -1, transport_error);
}

fl_response *fl_proto_execute_counted(fl_dbc *dbc, const char *sql, SQLLEN multi_statement_count,
                                      char **transport_error) {
    return fl_proto_execute_as(dbc, sql, multi_statement_count,
                               FL_EXEC_TRACKED | FL_EXEC_RECOVER, transport_error);
}

/* The most, in seconds, that releasing the session may take on SQLDisconnect. */
#define RELEASE_LIMIT 5

void fl_proto_release(fl_dbc *dbc) {
    if (dbc->session_id != NULL && dbc->tracks_sessions == 1) {
        int limit = RELEASE_LIMIT;
        if (dbc->login_timeout > 0 && dbc->login_timeout < limit) {
            limit = dbc->login_timeout;
        }
        if (dbc->request_timeout > 0 && dbc->request_timeout < limit) {
            limit = dbc->request_timeout;
        }
        /* The id goes in the path as one segment: everything but the unreserved characters is
         * escaped. */
        fl_strbuf path;
        fl_strbuf_init(&path);
        int failed = fl_strbuf_append(&path, "/api/sessions/") != 0;
        for (const unsigned char *c = (const unsigned char *) dbc->session_id; !failed && *c; c++) {
            if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')
                || *c == '-' || *c == '.' || *c == '_' || *c == '~') {
                failed = fl_strbuf_append_len(&path, (const char *) c, 1) != 0;
            } else {
                char escaped[4];
                snprintf(escaped, sizeof(escaped), "%%%02X", *c);
                failed = fl_strbuf_append(&path, escaped) != 0;
            }
        }
        if (!failed) {
            /* What it answers does not matter: a 404 means the session had already gone, and
             * anything else is not the disconnect's to fix. */
            int status = 0;
            char *body = NULL;
            char *error = NULL;
            if (fl_http_delete(dbc->host, dbc->port, path.data, limit, &status, &body, &error) == 0) {
                free(body);
            }
            free(error);
        }
        fl_strbuf_free(&path);
    }
    free(dbc->session_id);
    dbc->session_id = NULL;
}
