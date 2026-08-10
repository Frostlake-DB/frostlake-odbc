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
 * Internal model of the Frostlake ODBC driver.
 *
 * The driver is a thin native client for the engine's HTTP protocol
 * (POST /api/execute): every execution sends one JSON request and receives the
 * fully materialised result sets back, so a statement handle is just a cursor
 * over decoded JSON. Cells are kept as UTF-8 text exactly as they crossed the
 * wire (numbers keep their original lexeme, so NUMBER(38,…) values are not
 * squeezed through a double); conversions happen on demand in SQLGetData.
 */
#ifndef FL_ODBC_H
#define FL_ODBC_H

#include <sql.h>
#include <sqlext.h>

#include <stddef.h>

#define FL_DRIVER_NAME "Frostlake ODBC Driver"
#define FL_DRIVER_VER "01.00.0000"
#define FL_ODBC_VER "03.51"
#define FL_DBMS_NAME "Frostlake"

#define FL_MAX_DIAG_MSG 2048

/* ---- diagnostics ---------------------------------------------------------- */

typedef struct fl_diag {
    char sqlstate[6];
    char message[FL_MAX_DIAG_MSG];
    SQLINTEGER native_error;
    int present;
} fl_diag;

/* Every handle starts with its ODBC handle type so diag helpers can work on
 * any of them through a common prefix. */
typedef struct fl_handle_header {
    SQLSMALLINT handle_type;
    fl_diag diag;
} fl_handle_header;

/* ---- result model --------------------------------------------------------- */

/* One cell: wire text (UTF-8) or SQL NULL. `kind` records the JSON shape it
 * arrived as — 's'tring, 'n'umber, 'b'oolean — which conversions consult. */
typedef struct fl_cell {
    char *text;
    char kind;
    int is_null;
} fl_cell;

typedef struct fl_column {
    char *name;
    char *type_name;      /* engine wire name: NUMBER, VARCHAR, TIMESTAMP_NTZ, ... */
    SQLSMALLINT sql_type; /* mapped ODBC SQL_* type */
    SQLINTEGER precision; /* numeric precision from the wire, 0 otherwise */
    SQLINTEGER scale;     /* numeric scale from the wire */
} fl_column;

typedef struct fl_resultset {
    fl_column *columns;
    int column_count;
    fl_cell *cells; /* row-major: row * column_count + col */
    int row_count;
} fl_resultset;

/* The decoded SqlResponse: every result set of a (possibly multi-statement)
 * execution, in order. */
typedef struct fl_response {
    fl_resultset *sets;
    int set_count;
    char *session_id;
    char *error_message; /* non-NULL means success=false */
} fl_response;

void fl_response_free(fl_response *response);
void fl_resultset_free_contents(fl_resultset *set);

/* Builder used for locally synthesised result sets (SQLGetTypeInfo, catalog
 * shapes). Cells are added row-major with fl_synth_text/fl_synth_int/fl_synth_null. */
fl_resultset *fl_synth_new(int column_count, int row_capacity);
void fl_synth_column(fl_resultset *set, int index, const char *name, SQLSMALLINT sql_type);
int fl_synth_add_row(fl_resultset *set);
void fl_synth_text(fl_resultset *set, int row, int col, const char *text);
void fl_synth_int(fl_resultset *set, int row, int col, long value);
void fl_synth_null(fl_resultset *set, int row, int col);

/* ---- handles -------------------------------------------------------------- */

typedef struct fl_env {
    fl_handle_header hdr;
    SQLINTEGER odbc_version;
} fl_env;

typedef struct fl_dbc {
    fl_handle_header hdr;
    fl_env *env;
    int connected;
    char *host;
    int port;
    char *session_id;   /* assigned by the server on the first exchange */
    char *database;     /* last known catalog, tracked from USE DATABASE */
    char *schema;       /* last known schema, tracked from USE SCHEMA */
    char *dsn;
    int autocommit;     /* SQL_ATTR_AUTOCOMMIT, mirrored to ALTER SESSION */
    int login_timeout;    /* seconds for the connect probe, 0 = none */
    int request_timeout;  /* seconds for each statement, 0 = none */
} fl_dbc;

typedef struct fl_bound_col {
    SQLSMALLINT c_type;
    SQLPOINTER buffer;
    SQLLEN buffer_length;
    SQLLEN *str_len_or_ind;
    int bound;
} fl_bound_col;

typedef struct fl_bound_param {
    SQLSMALLINT c_type;
    SQLSMALLINT sql_type;
    SQLPOINTER buffer;
    SQLLEN *str_len_or_ind;
    SQLLEN buffer_length;
    int bound;
} fl_bound_param;

typedef struct fl_stmt {
    fl_handle_header hdr;
    fl_dbc *dbc;

    char *prepared_sql;

    /* results of the last execution */
    fl_response *response;
    int current_set;        /* index into response->sets */
    fl_resultset *synth;    /* locally built set (catalog functions) — owned */
    int current_row;        /* -1 before the first SQLFetch */
    SQLLEN row_count;       /* SQLRowCount value for the current result */
    int getdata_col;        /* per-cell offset tracking for chunked SQLGetData */
    size_t getdata_offset;

    fl_bound_col *bound_cols;
    int bound_col_capacity;

    fl_bound_param *params;
    int param_capacity;

    /* statement attributes this driver actually acts on */
    SQLULEN max_rows;               /* 0 = every row */
    SQLULEN *rows_fetched_ptr;      /* written by SQLFetch when set */
    SQLUSMALLINT *row_status_ptr;   /* written by SQLFetch when set */
} fl_stmt;

/* ---- diag helpers --------------------------------------------------------- */

void fl_diag_clear(SQLHANDLE handle);
SQLRETURN fl_diag_set(SQLHANDLE handle, const char *sqlstate, const char *message);
SQLRETURN fl_diag_setf(SQLHANDLE handle, const char *sqlstate, const char *fmt, ...);
/* Records a diagnostic that is a warning rather than a failure, and so answers
 * SQL_SUCCESS_WITH_INFO where fl_diag_set answers SQL_ERROR. */
SQLRETURN fl_diag_warn(SQLHANDLE handle, const char *sqlstate, const char *message);

/* ---- protocol (proto.c) --------------------------------------------------- */

/* Execute SQL over the connection's HTTP endpoint. On transport failure returns
 * NULL and fills *transport_error (caller frees). On success returns the decoded
 * response — which may still carry error_message (SQL failure). Updates the
 * connection's session id from the response. */
fl_response *fl_proto_execute(fl_dbc *dbc, const char *sql, char **transport_error);

/* ---- shared helpers ------------------------------------------------------- */

char *fl_strdup(const char *s);

/* Append a name as a SQL identifier that cannot break out of the statement. */
struct fl_strbuf;
int fl_strbuf_append_ident(struct fl_strbuf *buf, const char *name);
SQLSMALLINT fl_sql_type_for(const char *type_name, SQLINTEGER scale);
const char *fl_type_display_size(const fl_column *col, SQLULEN *column_size, SQLSMALLINT *decimal_digits);

/* The result set the statement currently exposes (wire or synthetic), or NULL. */
fl_resultset *fl_stmt_current_set(fl_stmt *stmt);
/* Reset cursor + bindings state for a new execution (keeps prepared SQL). */
void fl_stmt_clear_results(fl_stmt *stmt);
/* Sum of "number of rows ..." columns in the first row, or -1 if none. */
SQLLEN fl_rows_affected(const fl_resultset *set);

/* Copy text into an ODBC char output buffer with the standard truncation
 * contract; returns SQL_SUCCESS or SQL_SUCCESS_WITH_INFO. */
SQLRETURN fl_copy_string(SQLHANDLE diag_handle, const char *value,
                         SQLCHAR *buffer, SQLSMALLINT buffer_length,
                         SQLSMALLINT *out_length);

#endif /* FL_ODBC_H */
