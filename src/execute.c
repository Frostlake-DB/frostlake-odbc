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

/* Statement execution.
 *
 * Parameters are substituted client-side, exactly like the JDBC transport:
 * a `?` outside string literals becomes a SQL literal rendered from the bound
 * C value. Strings escape BOTH backslash and single quote (the engine's lexer
 * honors backslash escapes, so doubling only the quote would let a trailing
 * backslash break out of the literal); temporals are emitted in ISO form with
 * an explicit cast; binary binds as X'hex'.
 */

#include "fl_odbc.h"
#include "json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers -------------------------------------------------------------- */

static char *copy_sql(SQLCHAR *text, SQLINTEGER length) {
    size_t n = length == SQL_NTS ? strlen((const char *) text) : (size_t) length;
    char *sql = malloc(n + 1);
    if (sql != NULL) {
        memcpy(sql, text, n);
        sql[n] = '\0';
    }
    return sql;
}

/* If `c` opens a stretch of SQL in which a `?` is text rather than a
 * placeholder, return the position just past that stretch; otherwise NULL.
 *
 * The set matches the other Frostlake drivers: single-quoted strings (where a
 * backslash escapes the next character and '' is a doubled quote), quoted
 * identifiers, -- // and slash-star comments, and $$-delimited bodies, which is
 * how UDF and procedure sources are written — a `?` inside a JavaScript body is
 * the conditional operator, not a bind marker. An unterminated construct
 * swallows the rest of the statement, which is what the siblings do too. */
static const char *skip_non_placeholder(const char *c) {
    if (*c == '\'') {
        const char *p = c + 1;
        while (*p != '\0') {
            if (*p == '\\' && p[1] != '\0') {
                p += 2;
            } else if (*p == '\'') {
                if (p[1] != '\'') {
                    return p + 1;
                }
                p += 2;
            } else {
                p++;
            }
        }
        return p;
    }
    if (*c == '"') {
        const char *p = c + 1;
        while (*p != '\0') {
            if (*p == '"') {
                if (p[1] != '"') {
                    return p + 1;
                }
                p += 2;
            } else {
                p++;
            }
        }
        return p;
    }
    if ((c[0] == '-' && c[1] == '-') || (c[0] == '/' && c[1] == '/')) {
        const char *p = strchr(c + 2, '\n');
        return p != NULL ? p + 1 : c + strlen(c);
    }
    if (c[0] == '/' && c[1] == '*') {
        const char *p = strstr(c + 2, "*/");
        return p != NULL ? p + 2 : c + strlen(c);
    }
    if (c[0] == '$' && c[1] == '$') {
        const char *p = strstr(c + 2, "$$");
        return p != NULL ? p + 2 : c + strlen(c);
    }
    return NULL;
}

/* Whether the application has bound any parameter on this statement. Substitution waits for
 * one: with nothing bound, every `?` is the engine's to read — a Snowflake Scripting cursor bind
 * such as `DECLARE c CURSOR FOR ... WHERE x > ?` opened `USING (...)` is exactly that — and the
 * text goes through untouched, as a statement with no parameters does in the account's drivers.
 * Once something is bound, a marker without a binding is still refused with 07002. */
static int has_bound_params(const fl_stmt *stmt) {
    for (int i = 0; i < stmt->param_capacity; i++) {
        if (stmt->params[i].bound) {
            return 1;
        }
    }
    return 0;
}

/* Count the `?` markers that are actually placeholders. */
static int count_placeholders(const char *sql) {
    int count = 0;
    const char *c = sql;
    while (*c != '\0') {
        const char *past = skip_non_placeholder(c);
        if (past != NULL) {
            c = past;
            continue;
        }
        if (*c == '?') {
            count++;
        }
        c++;
    }
    return count;
}

/* Whether `text` is exactly a SQL numeric literal: optional sign, digits with
 * at most one point, optional exponent, and nothing else. Surrounding blanks
 * are tolerated because callers often bind a padded fixed-width buffer. */
static int is_numeric_literal(const char *text, size_t length) {
    size_t i = 0;
    size_t end = length;
    while (i < end && (text[i] == ' ' || text[i] == '\t')) {
        i++;
    }
    while (end > i && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        end--;
    }
    if (i == end) {
        return 0;
    }
    if (text[i] == '+' || text[i] == '-') {
        i++;
    }
    int digits = 0;
    while (i < end && text[i] >= '0' && text[i] <= '9') {
        i++;
        digits++;
    }
    if (i < end && text[i] == '.') {
        i++;
        while (i < end && text[i] >= '0' && text[i] <= '9') {
            i++;
            digits++;
        }
    }
    if (digits == 0) {
        return 0;
    }
    if (i < end && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < end && (text[i] == '+' || text[i] == '-')) {
            i++;
        }
        int exponent_digits = 0;
        while (i < end && text[i] >= '0' && text[i] <= '9') {
            i++;
            exponent_digits++;
        }
        if (exponent_digits == 0) {
            return 0;
        }
    }
    return i == end;
}

/* Append a string parameter as a quoted literal, escaping backslash + quote. */
static int append_string_literal(fl_strbuf *buf, const char *text, size_t length) {
    if (fl_strbuf_append(buf, "'") != 0) {
        return -1;
    }
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '\'' || c == '\\') {
            char pair[3] = { c == '\'' ? '\'' : '\\', c, '\0' };
            if (fl_strbuf_append_len(buf, pair, 2) != 0) {
                return -1;
            }
        } else if (fl_strbuf_append_len(buf, &c, 1) != 0) {
            return -1;
        }
    }
    return fl_strbuf_append(buf, "'");
}

static int append_binary_literal(fl_strbuf *buf, const unsigned char *bytes, size_t length) {
    if (fl_strbuf_append(buf, "X'") != 0) {
        return -1;
    }
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length; i++) {
        char pair[2] = { hex[bytes[i] >> 4], hex[bytes[i] & 0x0F] };
        if (fl_strbuf_append_len(buf, pair, 2) != 0) {
            return -1;
        }
    }
    return fl_strbuf_append(buf, "'");
}

/* Render one bound parameter as a SQL literal. */
static int append_param_literal(fl_stmt *stmt, fl_strbuf *buf, const fl_bound_param *param) {
    if (!param->bound) {
        fl_diag_set(stmt, "07002", "Statement has an unbound parameter");
        return -1;
    }
    SQLLEN indicator = param->str_len_or_ind != NULL ? *param->str_len_or_ind : SQL_NTS;
    if (indicator == SQL_NULL_DATA || param->buffer == NULL) {
        return fl_strbuf_append(buf, "NULL");
    }
    /* Past this point the indicator is used as a length, and SQL_NTS is the only
     * negative that still means one. Every other negative is a marker — the
     * data-at-execution pair, SQL_DEFAULT_PARAM, SQL_NO_TOTAL — and casting one
     * to size_t asks for a read of nearly the whole address space, which is a
     * segfault rather than an error. Refuse them by name instead. */
    if (indicator < 0 && indicator != SQL_NTS) {
        if (indicator == SQL_DATA_AT_EXEC || indicator <= SQL_LEN_DATA_AT_EXEC_OFFSET) {
            fl_diag_set(stmt, "HYC00", "Data-at-execution parameters are not supported");
        } else if (indicator == SQL_DEFAULT_PARAM) {
            fl_diag_set(stmt, "HYC00", "SQL_DEFAULT_PARAM is not supported");
        } else {
            fl_diag_setf(stmt, "HY090", "Invalid string or buffer length %ld", (long) indicator);
        }
        return -1;
    }

    char text[64];
    switch (param->c_type) {
        case SQL_C_CHAR: {
            size_t length = indicator == SQL_NTS ? strlen((const char *) param->buffer)
                                                 : (size_t) indicator;
            /* a character parameter bound as a temporal SQL type keeps its cast */
            if (param->sql_type == SQL_TYPE_DATE) {
                if (append_string_literal(buf, (const char *) param->buffer, length) != 0) return -1;
                return fl_strbuf_append(buf, "::DATE");
            }
            if (param->sql_type == SQL_TYPE_TIME) {
                if (append_string_literal(buf, (const char *) param->buffer, length) != 0) return -1;
                return fl_strbuf_append(buf, "::TIME");
            }
            if (param->sql_type == SQL_TYPE_TIMESTAMP) {
                if (append_string_literal(buf, (const char *) param->buffer, length) != 0) return -1;
                return fl_strbuf_append(buf, "::TIMESTAMP_NTZ");
            }
            if (param->sql_type == SQL_DECIMAL || param->sql_type == SQL_NUMERIC
                || param->sql_type == SQL_INTEGER || param->sql_type == SQL_BIGINT
                || param->sql_type == SQL_DOUBLE || param->sql_type == SQL_FLOAT
                || param->sql_type == SQL_REAL || param->sql_type == SQL_SMALLINT
                || param->sql_type == SQL_TINYINT) {
                /* Numeric-as-text binds unquoted so it stays a number, which
                 * means the buffer lands in the statement as SQL. Only an
                 * actual numeric literal may do that: without this check a
                 * parameter of "0 OR 1=1" was a bind-shaped injection. */
                if (!is_numeric_literal((const char *) param->buffer, length)) {
                    fl_diag_set(stmt, "22018",
                        "Parameter bound as a numeric SQL type is not a number");
                    return -1;
                }
                return fl_strbuf_append_len(buf, (const char *) param->buffer, length);
            }
            return append_string_literal(buf, (const char *) param->buffer, length);
        }
        case SQL_C_WCHAR:
            fl_diag_set(stmt, "HYC00", "Wide-character parameters are not supported; bind SQL_C_CHAR (UTF-8)");
            return -1;
        case SQL_C_SLONG:
        case SQL_C_LONG:
            snprintf(text, sizeof(text), "%ld", (long) *(SQLINTEGER *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_ULONG:
            snprintf(text, sizeof(text), "%lu", (unsigned long) *(SQLUINTEGER *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_SSHORT:
        case SQL_C_SHORT:
            snprintf(text, sizeof(text), "%d", (int) *(SQLSMALLINT *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_USHORT:
            snprintf(text, sizeof(text), "%u", (unsigned) *(SQLUSMALLINT *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_STINYINT:
        case SQL_C_TINYINT:
            snprintf(text, sizeof(text), "%d", (int) *(SQLSCHAR *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_UTINYINT:
            snprintf(text, sizeof(text), "%u", (unsigned) *(SQLCHAR *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_SBIGINT:
            snprintf(text, sizeof(text), "%lld", (long long) *(SQLBIGINT *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_UBIGINT:
            snprintf(text, sizeof(text), "%llu", (unsigned long long) *(SQLUBIGINT *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_FLOAT:
            snprintf(text, sizeof(text), "%.9g", (double) *(SQLREAL *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_DOUBLE:
            snprintf(text, sizeof(text), "%.17g", *(SQLDOUBLE *) param->buffer);
            return fl_strbuf_append(buf, text);
        case SQL_C_BIT:
            return fl_strbuf_append(buf, *(SQLCHAR *) param->buffer ? "TRUE" : "FALSE");
        case SQL_C_BINARY: {
            size_t length = indicator >= 0 ? (size_t) indicator : 0;
            return append_binary_literal(buf, (const unsigned char *) param->buffer, length);
        }
        case SQL_C_TYPE_DATE:
        case SQL_C_DATE: {
            const SQL_DATE_STRUCT *date = param->buffer;
            snprintf(text, sizeof(text), "'%04d-%02d-%02d'::DATE",
                     date->year, date->month, date->day);
            return fl_strbuf_append(buf, text);
        }
        case SQL_C_TYPE_TIME:
        case SQL_C_TIME: {
            const SQL_TIME_STRUCT *time = param->buffer;
            snprintf(text, sizeof(text), "'%02d:%02d:%02d'::TIME",
                     time->hour, time->minute, time->second);
            return fl_strbuf_append(buf, text);
        }
        case SQL_C_TYPE_TIMESTAMP:
        case SQL_C_TIMESTAMP: {
            const SQL_TIMESTAMP_STRUCT *ts = param->buffer;
            snprintf(text, sizeof(text), "'%04d-%02d-%02dT%02d:%02d:%02d.%09u'::TIMESTAMP_NTZ",
                     ts->year, ts->month, ts->day, ts->hour, ts->minute, ts->second,
                     (unsigned) ts->fraction);
            return fl_strbuf_append(buf, text);
        }
        default:
            fl_diag_setf(stmt, "HYC00", "Unsupported parameter C type %d", (int) param->c_type);
            return -1;
    }
}

/* Substitute every top-level `?` with its bound literal. */
static char *substitute_params(fl_stmt *stmt, const char *sql) {
    fl_strbuf buf;
    fl_strbuf_init(&buf);
    int param_index = 0;
    const char *c = sql;
    while (*c != '\0') {
        const char *past = skip_non_placeholder(c);
        if (past != NULL) {
            if (fl_strbuf_append_len(&buf, c, (size_t) (past - c)) != 0) goto fail;
            c = past;
            continue;
        }
        if (*c == '?') {
            if (param_index >= stmt->param_capacity) {
                fl_diag_set(stmt, "07002", "More placeholders than bound parameters");
                goto fail;
            }
            if (append_param_literal(stmt, &buf, &stmt->params[param_index]) != 0) {
                goto fail;
            }
            param_index++;
            c++;
            continue;
        }
        if (fl_strbuf_append_len(&buf, c, 1) != 0) goto fail;
        c++;
    }
    return buf.data;
fail:
    fl_strbuf_free(&buf);
    return NULL;
}

fl_resultset *fl_stmt_current_set(fl_stmt *stmt) {
    if (stmt->synth != NULL) {
        return stmt->synth;
    }
    if (stmt->response == NULL || stmt->current_set >= stmt->response->set_count) {
        return NULL;
    }
    return &stmt->response->sets[stmt->current_set];
}

/* Track USE DATABASE/SCHEMA the way the JDBC statement does, so
 * SQL_ATTR_CURRENT_CATALOG and SQLGetInfo(SQL_DATABASE_NAME) stay truthful. */
static void track_context(fl_dbc *dbc, const char *sql) {
    while (*sql == ' ' || *sql == '\t' || *sql == '\n' || *sql == '\r') {
        sql++;
    }
    const char *rest = NULL;
    char **slot = NULL;
    if (strncasecmp(sql, "USE DATABASE ", 13) == 0) {
        rest = sql + 13;
        slot = &dbc->database;
    } else if (strncasecmp(sql, "USE SCHEMA ", 11) == 0) {
        rest = sql + 11;
        slot = &dbc->schema;
    } else {
        return;
    }
    while (*rest == ' ') {
        rest++;
    }
    size_t length = strlen(rest);
    while (length > 0 && (rest[length - 1] == ';' || rest[length - 1] == ' '
                          || rest[length - 1] == '\n' || rest[length - 1] == '\r')) {
        length--;
    }
    if (length == 0) {
        return;
    }
    char *name = malloc(length + 1);
    if (name == NULL) {
        return;
    }
    if (rest[0] == '"' && length >= 2 && rest[length - 1] == '"') {
        /* A quoted name is already the object's real name: keep its case and
         * undouble the interior quotes. Upper-casing it turned USE DATABASE
         * "mixedCase" into a current catalog of "MIXEDCASE", quotes and all. */
        size_t out = 0;
        for (size_t i = 1; i + 1 < length; i++) {
            if (rest[i] == '"' && rest[i + 1] == '"') {
                i++;
            }
            name[out++] = rest[i];
        }
        name[out] = '\0';
    } else {
        /* A bare name is folded by the engine, so track the folded form. */
        for (size_t i = 0; i < length; i++) {
            name[i] = rest[i] >= 'a' && rest[i] <= 'z' ? (char) (rest[i] - 'a' + 'A') : rest[i];
        }
        name[length] = '\0';
    }
    free(*slot);
    *slot = name;
}

/* Shared execution path for SQLExecDirect and SQLExecute. */
static SQLRETURN run_sql(fl_stmt *stmt, const char *sql) {
    fl_stmt_clear_results(stmt);
    char *transport_error = NULL;
    fl_response *response = fl_proto_execute_counted(stmt->dbc, sql, stmt->multi_statement_count,
                                                    &transport_error);
    if (response == NULL) {
        SQLRETURN rc = fl_diag_setf(stmt, "08S01", "%s", transport_error);
        free(transport_error);
        return rc;
    }
    if (response->error_message != NULL) {
        SQLRETURN rc = fl_diag_setf(stmt, "42000", "%s", response->error_message);
        fl_response_free(response);
        return rc;
    }
    stmt->response = response;
    stmt->current_set = 0;
    stmt->current_row = -1;
    track_context(stmt->dbc, sql);

    fl_resultset *set = fl_stmt_current_set(stmt);
    if (set != NULL) {
        SQLLEN affected = fl_rows_affected(set);
        stmt->row_count = affected >= 0 ? affected : set->row_count;
    } else {
        stmt->row_count = 0;
    }
    return SQL_SUCCESS;
}

/* ---- API ------------------------------------------------------------------- */

SQLRETURN SQL_API SQLExecDirect(SQLHSTMT StatementHandle, SQLCHAR *StatementText,
                                SQLINTEGER TextLength) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    char *sql = copy_sql(StatementText, TextLength);
    if (sql == NULL) {
        return fl_diag_set(stmt, "HY001", "Out of memory");
    }
    char *final_sql = sql;
    if (has_bound_params(stmt) && count_placeholders(sql) > 0) {
        final_sql = substitute_params(stmt, sql);
        if (final_sql == NULL) {
            free(sql);
            return SQL_ERROR; /* diag already set */
        }
    }
    SQLRETURN rc = run_sql(stmt, final_sql);
    if (final_sql != sql) {
        free(final_sql);
    }
    free(sql);
    return rc;
}

SQLRETURN SQL_API SQLPrepare(SQLHSTMT StatementHandle, SQLCHAR *StatementText,
                             SQLINTEGER TextLength) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    free(stmt->prepared_sql);
    stmt->prepared_sql = copy_sql(StatementText, TextLength);
    if (stmt->prepared_sql == NULL) {
        return fl_diag_set(stmt, "HY001", "Out of memory");
    }
    fl_stmt_clear_results(stmt);
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLExecute(SQLHSTMT StatementHandle) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    if (stmt->prepared_sql == NULL) {
        return fl_diag_set(stmt, "HY010", "No prepared statement");
    }
    char *final_sql = stmt->prepared_sql;
    int substituted = 0;
    if (has_bound_params(stmt) && count_placeholders(stmt->prepared_sql) > 0) {
        final_sql = substitute_params(stmt, stmt->prepared_sql);
        if (final_sql == NULL) {
            return SQL_ERROR; /* diag already set */
        }
        substituted = 1;
    }
    SQLRETURN rc = run_sql(stmt, final_sql);
    if (substituted) {
        free(final_sql);
    }
    return rc;
}

SQLRETURN SQL_API SQLBindParameter(SQLHSTMT StatementHandle, SQLUSMALLINT ParameterNumber,
                                   SQLSMALLINT InputOutputType, SQLSMALLINT ValueType,
                                   SQLSMALLINT ParameterType, SQLULEN ColumnSize,
                                   SQLSMALLINT DecimalDigits, SQLPOINTER ParameterValuePtr,
                                   SQLLEN BufferLength, SQLLEN *StrLen_or_IndPtr) {
    (void) ColumnSize;
    (void) DecimalDigits;
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    if (InputOutputType != SQL_PARAM_INPUT) {
        return fl_diag_set(stmt, "HYC00", "Only input parameters are supported");
    }
    if (ParameterNumber < 1) {
        return fl_diag_set(stmt, "07009", "Invalid parameter number");
    }
    if (ParameterNumber > stmt->param_capacity) {
        fl_bound_param *grown = realloc(stmt->params,
                                        ParameterNumber * sizeof(fl_bound_param));
        if (grown == NULL) {
            return fl_diag_set(stmt, "HY001", "Out of memory");
        }
        memset(grown + stmt->param_capacity, 0,
               (size_t) (ParameterNumber - stmt->param_capacity) * sizeof(fl_bound_param));
        stmt->params = grown;
        stmt->param_capacity = ParameterNumber;
    }
    fl_bound_param *param = &stmt->params[ParameterNumber - 1];
    param->c_type = ValueType == SQL_C_DEFAULT ? SQL_C_CHAR : ValueType;
    param->sql_type = ParameterType;
    param->buffer = ParameterValuePtr;
    param->buffer_length = BufferLength;
    param->str_len_or_ind = StrLen_or_IndPtr;
    param->bound = 1;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLNumParams(SQLHSTMT StatementHandle, SQLSMALLINT *ParameterCountPtr) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    if (ParameterCountPtr != NULL) {
        *ParameterCountPtr = stmt->prepared_sql != NULL
            ? (SQLSMALLINT) count_placeholders(stmt->prepared_sql)
            : 0;
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLRowCount(SQLHSTMT StatementHandle, SQLLEN *RowCountPtr) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    if (RowCountPtr != NULL) {
        *RowCountPtr = stmt->row_count;
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLMoreResults(SQLHSTMT StatementHandle) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    if (stmt->synth != NULL || stmt->response == NULL) {
        return SQL_NO_DATA;
    }
    if (stmt->current_set + 1 >= stmt->response->set_count) {
        return SQL_NO_DATA;
    }
    stmt->current_set++;
    stmt->current_row = -1;
    stmt->getdata_col = -1;
    stmt->getdata_offset = 0;
    fl_resultset *set = fl_stmt_current_set(stmt);
    SQLLEN affected = fl_rows_affected(set);
    stmt->row_count = affected >= 0 ? affected : set->row_count;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLCloseCursor(SQLHSTMT StatementHandle) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    fl_stmt_clear_results(stmt);
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLCancel(SQLHSTMT StatementHandle) {
    /* execution is synchronous: by the time anyone can call this, the
     * statement has already returned */
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    return SQL_SUCCESS;
}

/* ---- statement attributes -------------------------------------------------- */

SQLRETURN SQL_API SQLSetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER Attribute,
                                 SQLPOINTER ValuePtr, SQLINTEGER StringLength) {
    (void) StringLength;
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    SQLULEN value = (SQLULEN) (uintptr_t) ValuePtr;
    switch (Attribute) {
        case SQL_ATTR_MAX_ROWS:
            /* honoured for real in SQLFetch */
            stmt->max_rows = (SQLULEN) value;
            return SQL_SUCCESS;
        case SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT:
            /* How many statements the next execute carries: -1 leaves it to the session, 0 allows
             * any number. It rides on the request and changes nothing about the session. */
            if ((SQLLEN) (intptr_t) ValuePtr < -1) {
                return fl_diag_set(stmt, "HY024",
                    "SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT must be -1, 0, or a statement count");
            }
            stmt->multi_statement_count = (SQLLEN) (intptr_t) ValuePtr;
            return SQL_SUCCESS;
        case SQL_ATTR_ROWS_FETCHED_PTR:
            stmt->rows_fetched_ptr = (SQLULEN *) ValuePtr;
            return SQL_SUCCESS;
        case SQL_ATTR_ROW_STATUS_PTR:
            stmt->row_status_ptr = (SQLUSMALLINT *) ValuePtr;
            return SQL_SUCCESS;
        case SQL_ATTR_ROW_ARRAY_SIZE:
        case SQL_ROWSET_SIZE:
            /* Refused rather than warned about: an application that believes
             * this took effect sizes its buffers for `value` rows and reads the
             * ones this driver never writes. */
            if (value == 1) {
                return SQL_SUCCESS;
            }
            return fl_diag_set(stmt, "HYC00",
                "Row-wise array fetching is not supported; SQL_ATTR_ROW_ARRAY_SIZE must be 1");
        case SQL_ATTR_CURSOR_TYPE:
            if (value == SQL_CURSOR_FORWARD_ONLY) {
                return SQL_SUCCESS;
            }
            return fl_diag_warn(stmt, "01S02", "Cursor type changed to forward-only");
        case SQL_ATTR_CURSOR_SCROLLABLE:
            if (value == SQL_NONSCROLLABLE) {
                return SQL_SUCCESS;
            }
            return fl_diag_warn(stmt, "01S02", "Cursor changed to non-scrollable");
        case SQL_ATTR_CONCURRENCY:
            if (value == SQL_CONCUR_READ_ONLY) {
                return SQL_SUCCESS;
            }
            return fl_diag_warn(stmt, "01S02", "Concurrency changed to read-only");
        case SQL_ATTR_QUERY_TIMEOUT:
            if (value == 0) {
                return SQL_SUCCESS;
            }
            return fl_diag_warn(stmt, "01S02",
                "Per-statement query timeout is not supported; changed to 0");
        case SQL_ATTR_ROW_BIND_TYPE:
            if (value == SQL_BIND_BY_COLUMN) {
                return SQL_SUCCESS;
            }
            return fl_diag_set(stmt, "HYC00", "Only column-wise binding is supported");
        case SQL_ATTR_RETRIEVE_DATA:
            if (value == SQL_RD_ON) {
                return SQL_SUCCESS;
            }
            return fl_diag_set(stmt, "HYC00", "SQL_ATTR_RETRIEVE_DATA cannot be turned off");
        case SQL_ATTR_NOSCAN:
        case SQL_ATTR_APP_ROW_DESC:
        case SQL_ATTR_APP_PARAM_DESC:
        case SQL_ATTR_IMP_ROW_DESC:
        case SQL_ATTR_IMP_PARAM_DESC:
        case SQL_ATTR_METADATA_ID:
        case SQL_ATTR_ENABLE_AUTO_IPD:
        case SQL_ATTR_PARAM_BIND_TYPE:
        case SQL_ATTR_PARAMSET_SIZE:
        case SQL_ATTR_PARAMS_PROCESSED_PTR:
        case SQL_ATTR_PARAM_STATUS_PTR:
        case SQL_ATTR_PARAM_OPERATION_PTR:
        case SQL_ATTR_ROW_OPERATION_PTR:
        case SQL_ATTR_FETCH_BOOKMARK_PTR:
        case SQL_ATTR_USE_BOOKMARKS:
        case SQL_ATTR_SIMULATE_CURSOR:
        case SQL_ATTR_KEYSET_SIZE:
        case SQL_ATTR_MAX_LENGTH:
            /* Accepted as no-ops: the driver manager sets several of these on
             * every statement, and none of them changes what this driver does. */
            return SQL_SUCCESS;
        default:
            return fl_diag_setf(stmt, "HY092",
                "Unsupported statement attribute %ld", (long) Attribute);
    }
}

SQLRETURN SQL_API SQLGetStmtAttr(SQLHSTMT StatementHandle, SQLINTEGER Attribute,
                                 SQLPOINTER ValuePtr, SQLINTEGER BufferLength,
                                 SQLINTEGER *StringLengthPtr) {
    (void) BufferLength;
    (void) StringLengthPtr;
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    if (ValuePtr == NULL) {
        return SQL_SUCCESS;
    }
    switch (Attribute) {
        case SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT:
            *(SQLLEN *) ValuePtr = stmt->multi_statement_count;
            return SQL_SUCCESS;
        case SQL_ATTR_CURSOR_TYPE:
            *(SQLULEN *) ValuePtr = SQL_CURSOR_FORWARD_ONLY;
            return SQL_SUCCESS;
        case SQL_ATTR_CONCURRENCY:
            *(SQLULEN *) ValuePtr = SQL_CONCUR_READ_ONLY;
            return SQL_SUCCESS;
        case SQL_ATTR_ROW_ARRAY_SIZE:
            *(SQLULEN *) ValuePtr = 1;
            return SQL_SUCCESS;
        case SQL_ATTR_ROW_NUMBER:
            *(SQLULEN *) ValuePtr = stmt->current_row >= 0 ? (SQLULEN) stmt->current_row + 1 : 0;
            return SQL_SUCCESS;
        case SQL_ATTR_APP_ROW_DESC:
        case SQL_ATTR_APP_PARAM_DESC:
        case SQL_ATTR_IMP_ROW_DESC:
        case SQL_ATTR_IMP_PARAM_DESC:
            /* descriptors are not modelled; hand back the statement itself as
             * an opaque non-NULL token */
            *(SQLPOINTER *) ValuePtr = StatementHandle;
            return SQL_SUCCESS;
        default:
            *(SQLULEN *) ValuePtr = 0;
            return SQL_SUCCESS;
    }
}
