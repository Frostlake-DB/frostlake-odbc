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

#include "fl_odbc.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char *fl_strdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t length = strlen(s);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, s, length + 1);
    }
    return copy;
}

/* ---- identifiers ----------------------------------------------------------- */

/* A name that may stand in SQL without quotes, and so keeps the engine's usual
 * upper-casing. Deliberately the conservative set: anything else gets quoted. */
static int ident_is_bare(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z')
          || name[0] == '_')) {
        return 0;
    }
    for (const char *c = name + 1; *c != '\0'; c++) {
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')
              || (*c >= '0' && *c <= '9') || *c == '_' || *c == '$')) {
            return 0;
        }
    }
    return 1;
}

/* A well-formed quoted identifier: wrapped in quotes, every interior quote
 * doubled. Checked rather than assumed — `"a";DROP TABLE x--"` also begins and
 * ends with a quote, and passing that through is the hole this closes. */
static int ident_is_quoted(const char *name) {
    size_t length = name != NULL ? strlen(name) : 0;
    if (length < 2 || name[0] != '"' || name[length - 1] != '"') {
        return 0;
    }
    for (size_t i = 1; i + 1 < length; i++) {
        if (name[i] != '"') {
            continue;
        }
        if (i + 2 < length && name[i + 1] == '"') {
            i++;
            continue;
        }
        return 0;
    }
    return 1;
}

/* Append a caller-supplied name as an identifier that cannot escape into the
 * surrounding statement. Names that are already legal on their own are passed
 * through unchanged so existing DSNs keep resolving to the same object; only
 * the ones that could break out are quoted. */
int fl_strbuf_append_ident(fl_strbuf *buf, const char *name) {
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    if (ident_is_quoted(name) || ident_is_bare(name)) {
        return fl_strbuf_append(buf, name);
    }
    if (fl_strbuf_append(buf, "\"") != 0) {
        return -1;
    }
    for (const char *c = name; *c != '\0'; c++) {
        if (*c == '"' && fl_strbuf_append(buf, "\"") != 0) {
            return -1;
        }
        if (fl_strbuf_append_len(buf, c, 1) != 0) {
            return -1;
        }
    }
    return fl_strbuf_append(buf, "\"");
}

/*
 * Engine wire type name -> ODBC SQL type. Mirrors the JDBC driver's mapping,
 * including its order: the more specific name is tested before the shorter one
 * it contains (TIMESTAMP before DATE/TIME, BIGINT before INT). The one ODBC
 * refinement: a NUMBER with scale 0 surfaces as SQL_BIGINT, scaled as
 * SQL_DECIMAL.
 */
SQLSMALLINT fl_sql_type_for(const char *type_name, SQLINTEGER scale) {
    if (type_name == NULL) {
        return SQL_VARCHAR;
    }
    if (strcasestr(type_name, "TIMESTAMP") != NULL || strcasestr(type_name, "DATETIME") != NULL) {
        return SQL_TYPE_TIMESTAMP;
    }
    if (strcasestr(type_name, "DATE") != NULL) {
        return SQL_TYPE_DATE;
    }
    if (strcasestr(type_name, "TIME") != NULL) {
        return SQL_TYPE_TIME;
    }
    if (strcasestr(type_name, "INT") != NULL) {
        /* INT, INTEGER, BIGINT, SMALLINT, TINYINT: all Snowflake aliases for
         * NUMBER(38,0), stored by the engine as 64-bit values — SQL_BIGINT is
         * the truthful width for every one of them. */
        return SQL_BIGINT;
    }
    if (strcasestr(type_name, "DECIMAL") != NULL || strcasestr(type_name, "NUMBER") != NULL
        || strcasestr(type_name, "NUMERIC") != NULL) {
        return scale == 0 ? SQL_BIGINT : SQL_DECIMAL;
    }
    if (strcasestr(type_name, "DOUBLE") != NULL || strcasestr(type_name, "REAL") != NULL
        || strcasestr(type_name, "FLOAT") != NULL) {
        return SQL_DOUBLE;
    }
    if (strcasecmp(type_name, "BOOL") == 0 || strcasestr(type_name, "BOOLEAN") != NULL) {
        return SQL_BIT;
    }
    if (strcasestr(type_name, "BINARY") != NULL || strcasestr(type_name, "BYTES") != NULL) {
        return SQL_VARBINARY;
    }
    /* VARCHAR/CHAR/STRING/TEXT and the semi-structured family (VARIANT, OBJECT,
     * ARRAY, MAP, GEOGRAPHY, GEOMETRY, VECTOR, FILE) all cross as text. */
    return SQL_VARCHAR;
}

/*
 * Column size / decimal digits per ODBC conventions. Returns the type name to
 * report (currently the wire name itself).
 */
const char *fl_type_display_size(const fl_column *col, SQLULEN *column_size, SQLSMALLINT *decimal_digits) {
    SQLULEN size;
    SQLSMALLINT digits = 0;
    switch (col->sql_type) {
        case SQL_BIGINT:
            size = col->precision > 0 ? (SQLULEN) col->precision : 19;
            break;
        case SQL_DECIMAL:
            size = col->precision > 0 ? (SQLULEN) col->precision : 38;
            digits = (SQLSMALLINT) col->scale;
            break;
        case SQL_INTEGER: size = 10; break;
        case SQL_SMALLINT: size = 5; break;
        case SQL_TINYINT: size = 3; break;
        case SQL_DOUBLE: size = 15; break;
        case SQL_BIT: size = 1; break;
        case SQL_TYPE_DATE: size = 10; break;
        case SQL_TYPE_TIME: size = 8; break;
        case SQL_TYPE_TIMESTAMP:
            size = 29;
            /* What the engine actually renders is milliseconds, so that is what
             * DescribeCol reports. Claiming the Snowflake-nominal 9 promised a
             * precision no row ever arrives with. */
            digits = 3;
            break;
        case SQL_VARBINARY:
            /* The wire carries a binary column's own length in bytes; the constant is only the
             * fallback for an engine that predates the field. */
            size = col->length > 0 ? (SQLULEN) col->length : 8388608;
            break;
        default:
            /* Likewise for text, in characters. An unbounded VARCHAR reports the maximum, which is
             * what the account reports for one too, so the fallback and the real value agree there. */
            size = col->length > 0 ? (SQLULEN) col->length : 16777216;
            break;
    }
    if (column_size != NULL) {
        *column_size = size;
    }
    if (decimal_digits != NULL) {
        *decimal_digits = digits;
    }
    return col->type_name != NULL ? col->type_name : "VARCHAR";
}

/* Sum of the Snowflake-style DML report columns, mirroring the JDBC driver. */
SQLLEN fl_rows_affected(const fl_resultset *set) {
    if (set == NULL || set->row_count < 1) {
        return -1;
    }
    SQLLEN total = 0;
    int found = 0;
    for (int i = 0; i < set->column_count; i++) {
        const char *name = set->columns[i].name;
        if (name != NULL && strncasecmp(name, "number of rows", 14) == 0) {
            const fl_cell *cell = &set->cells[i];
            if (!cell->is_null && cell->text != NULL) {
                total += (SQLLEN) strtoll(cell->text, NULL, 10);
                found = 1;
            }
        }
    }
    return found ? total : -1;
}

/* ---- synthetic result sets ------------------------------------------------ */

fl_resultset *fl_synth_new(int column_count, int row_capacity) {
    fl_resultset *set = calloc(1, sizeof(fl_resultset));
    if (set == NULL) {
        return NULL;
    }
    set->column_count = column_count;
    set->columns = calloc((size_t) column_count, sizeof(fl_column));
    set->cells = calloc((size_t) (column_count * (row_capacity > 0 ? row_capacity : 1)), sizeof(fl_cell));
    set->row_count = 0;
    if (set->columns == NULL || set->cells == NULL) {
        fl_resultset_free_contents(set);
        free(set);
        return NULL;
    }
    return set;
}

void fl_synth_column(fl_resultset *set, int index, const char *name, SQLSMALLINT sql_type) {
    set->columns[index].name = fl_strdup(name);
    set->columns[index].sql_type = sql_type;
    set->columns[index].type_name = fl_strdup(
        sql_type == SQL_SMALLINT ? "SMALLINT"
        : sql_type == SQL_INTEGER ? "INTEGER"
        : "VARCHAR");
    set->columns[index].precision = 0;
    set->columns[index].scale = 0;
}

int fl_synth_add_row(fl_resultset *set) {
    /* rows were allocated up-front by fl_synth_new's row_capacity */
    set->row_count++;
    return set->row_count - 1;
}

void fl_synth_text(fl_resultset *set, int row, int col, const char *text) {
    fl_cell *cell = &set->cells[row * set->column_count + col];
    cell->text = fl_strdup(text);
    cell->kind = 's';
    cell->is_null = 0;
}

void fl_synth_int(fl_resultset *set, int row, int col, long value) {
    char text[24];
    snprintf(text, sizeof(text), "%ld", value);
    fl_cell *cell = &set->cells[row * set->column_count + col];
    cell->text = fl_strdup(text);
    cell->kind = 'n';
    cell->is_null = 0;
}

void fl_synth_null(fl_resultset *set, int row, int col) {
    fl_cell *cell = &set->cells[row * set->column_count + col];
    cell->text = NULL;
    cell->is_null = 1;
    cell->kind = 's';
}

void fl_resultset_free_contents(fl_resultset *set) {
    if (set == NULL) {
        return;
    }
    for (int i = 0; i < set->column_count; i++) {
        free(set->columns[i].name);
        free(set->columns[i].type_name);
    }
    free(set->columns);
    if (set->cells != NULL) {
        for (int i = 0; i < set->row_count * set->column_count; i++) {
            free(set->cells[i].text);
        }
        free(set->cells);
    }
    set->columns = NULL;
    set->cells = NULL;
    set->column_count = 0;
    set->row_count = 0;
}

void fl_response_free(fl_response *response) {
    if (response == NULL) {
        return;
    }
    for (int i = 0; i < response->set_count; i++) {
        fl_resultset_free_contents(&response->sets[i]);
    }
    free(response->sets);
    free(response->session_id);
    free(response->error_message);
    free(response);
}

/* ---- ODBC string output --------------------------------------------------- */

SQLRETURN fl_copy_string(SQLHANDLE diag_handle, const char *value,
                         SQLCHAR *buffer, SQLSMALLINT buffer_length,
                         SQLSMALLINT *out_length) {
    if (value == NULL) {
        value = "";
    }
    size_t length = strlen(value);
    if (out_length != NULL) {
        *out_length = (SQLSMALLINT) length;
    }
    if (buffer == NULL || buffer_length <= 0) {
        return SQL_SUCCESS;
    }
    if (length < (size_t) buffer_length) {
        memcpy(buffer, value, length + 1);
        return SQL_SUCCESS;
    }
    memcpy(buffer, value, (size_t) buffer_length - 1);
    buffer[buffer_length - 1] = '\0';
    if (diag_handle != NULL) {
        fl_diag_set(diag_handle, "01004", "String data, right truncated");
    }
    return SQL_SUCCESS_WITH_INFO;
}
