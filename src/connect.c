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

/* Connection establishment and connection-level metadata.
 *
 * A connection is host+port of a running DatabaseHttpServer plus the session
 * the server assigns on first contact. Connecting performs a /api/health probe
 * (so a bad address fails at SQLDriverConnect, not at the first statement) and
 * then issues USE DATABASE / USE SCHEMA when the connection string names them.
 */

#include "fl_odbc.h"
#include "http.h"
#include "json.h"

#include <odbcinst.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- connection string ---------------------------------------------------- */

/* Extract `key=value` (case-insensitive key) from an ODBC connection string.
 * Values may be brace-wrapped ({value}) per the spec. Returns malloc'd value
 * or NULL. */
static char *conn_str_get(const char *conn, const char *key) {
    size_t key_length = strlen(key);
    const char *cur = conn;
    while (cur != NULL && *cur != '\0') {
        while (*cur == ';' || *cur == ' ') {
            cur++;
        }
        const char *eq = strchr(cur, '=');
        if (eq == NULL) {
            break;
        }
        size_t this_key_length = (size_t) (eq - cur);
        const char *value = eq + 1;
        const char *end;
        if (*value == '{') {
            value++;
            end = strchr(value, '}');
            if (end == NULL) {
                end = value + strlen(value);
            }
        } else {
            end = strchr(value, ';');
            if (end == NULL) {
                end = value + strlen(value);
            }
        }
        if (this_key_length == key_length && strncasecmp(cur, key, key_length) == 0) {
            size_t value_length = (size_t) (end - value);
            char *result = malloc(value_length + 1);
            if (result != NULL) {
                memcpy(result, value, value_length);
                result[value_length] = '\0';
            }
            return result;
        }
        cur = *end == '}' ? strchr(end, ';') : end;
        if (cur != NULL && *cur == ';') {
            cur++;
        }
    }
    return NULL;
}

/* DSN attribute via the driver manager's ini machinery (odbc.ini). */
static char *dsn_get(const char *dsn, const char *key) {
    char value[512];
    value[0] = '\0';
    int n = SQLGetPrivateProfileString(dsn, key, "", value, sizeof(value), "odbc.ini");
    if (n <= 0 || value[0] == '\0') {
        return NULL;
    }
    return fl_strdup(value);
}

/* A TCP port, or -1. atoi() answered 0 for "notanumber" and let anything out of
 * range through to getaddrinfo, which truncates modulo 65536 — so Port=84042
 * quietly connected to 18506. */
static int parse_port(const char *text) {
    if (text == NULL) {
        return -1;
    }
    /* ini files and connection strings are written by hand, so a value arrives
     * padded often enough that refusing " 18082 " would be its own bug. */
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    const char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    if (end == text) {
        return -1;
    }
    long value = 0;
    for (const char *c = text; c < end; c++) {
        if (*c < '0' || *c > '9') {
            return -1;
        }
        value = value * 10 + (*c - '0');
        if (value > 65535) {
            return -1;
        }
    }
    return value >= 1 ? (int) value : -1;
}

/* ---- connect -------------------------------------------------------------- */

static SQLRETURN do_connect(fl_dbc *dbc, const char *database, const char *schema) {
    if (dbc->host == NULL) {
        dbc->host = fl_strdup("localhost");
    }

    int status = 0;
    char *body = NULL;
    char *error = NULL;
    if (fl_http_get(dbc->host, dbc->port, "/api/health", dbc->login_timeout,
                    &status, &body, &error) != 0) {
        SQLRETURN rc = fl_diag_setf(dbc, "08001",
            "Cannot reach Frostlake server at %s:%d (%s)", dbc->host, dbc->port,
            error != NULL ? error : "unknown error");
        free(error);
        return rc;
    }
    free(body);
    if (status != 200) {
        return fl_diag_setf(dbc, "08001",
            "Frostlake server at %s:%d answered HTTP %d to the health probe",
            dbc->host, dbc->port, status);
    }

    dbc->connected = 1;

    /* Pick the working database/schema up front, mirroring the JDBC URL path
     * segment. Failure here is a real connect failure (bad database name). */
    /* The name goes through the identifier quoter rather than straight into the
     * statement: a DSN value is caller-supplied text, and `USE DATABASE %s` let
     * it carry its own SQL. A fl_strbuf also removes the silent truncation the
     * old fixed 600-byte buffer did to a long name. */
    fl_strbuf use_sql;
    if (database != NULL && database[0] != '\0') {
        fl_strbuf_init(&use_sql);
        if (fl_strbuf_append(&use_sql, "USE DATABASE ") != 0
            || fl_strbuf_append_ident(&use_sql, database) != 0) {
            fl_strbuf_free(&use_sql);
            dbc->connected = 0;
            return fl_diag_set(dbc, "HY001", "Out of memory");
        }
        char *transport_error = NULL;
        fl_response *response = fl_proto_execute(dbc, use_sql.data, &transport_error);
        fl_strbuf_free(&use_sql);
        if (response == NULL) {
            dbc->connected = 0;
            SQLRETURN rc = fl_diag_setf(dbc, "08001", "%s", transport_error);
            free(transport_error);
            return rc;
        }
        if (response->error_message != NULL) {
            SQLRETURN rc = fl_diag_setf(dbc, "08004", "%s", response->error_message);
            fl_response_free(response);
            dbc->connected = 0;
            return rc;
        }
        fl_response_free(response);
        free(dbc->database);
        dbc->database = fl_strdup(database);
    }
    if (schema != NULL && schema[0] != '\0') {
        fl_strbuf_init(&use_sql);
        if (fl_strbuf_append(&use_sql, "USE SCHEMA ") != 0
            || fl_strbuf_append_ident(&use_sql, schema) != 0) {
            fl_strbuf_free(&use_sql);
            dbc->connected = 0;
            return fl_diag_set(dbc, "HY001", "Out of memory");
        }
        char *transport_error = NULL;
        fl_response *response = fl_proto_execute(dbc, use_sql.data, &transport_error);
        fl_strbuf_free(&use_sql);
        if (response == NULL || response->error_message != NULL) {
            SQLRETURN rc = fl_diag_setf(dbc, "08004", "%s",
                response != NULL ? response->error_message : transport_error);
            free(transport_error);
            fl_response_free(response);
            dbc->connected = 0;
            return rc;
        }
        fl_response_free(response);
        free(dbc->schema);
        dbc->schema = fl_strdup(schema);
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLConnect(SQLHDBC ConnectionHandle,
                             SQLCHAR *ServerName, SQLSMALLINT NameLength1,
                             SQLCHAR *UserName, SQLSMALLINT NameLength2,
                             SQLCHAR *Authentication, SQLSMALLINT NameLength3) {
    (void) UserName; (void) NameLength2; (void) Authentication; (void) NameLength3;
    fl_dbc *dbc = (fl_dbc *) ConnectionHandle;
    if (dbc == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(dbc);
    if (dbc->connected) {
        return fl_diag_set(dbc, "08002", "Connection already open");
    }

    char dsn[256];
    size_t dsn_length = NameLength1 == SQL_NTS
        ? strlen((const char *) ServerName)
        : (size_t) NameLength1;
    if (dsn_length >= sizeof(dsn)) {
        dsn_length = sizeof(dsn) - 1;
    }
    memcpy(dsn, ServerName, dsn_length);
    dsn[dsn_length] = '\0';
    free(dbc->dsn);
    dbc->dsn = fl_strdup(dsn);

    char *server = dsn_get(dsn, "Server");
    char *port = dsn_get(dsn, "Port");
    char *database = dsn_get(dsn, "Database");
    char *schema = dsn_get(dsn, "Schema");

    free(dbc->host);
    dbc->host = server != NULL ? server : fl_strdup("localhost");
    if (port != NULL) {
        int parsed = parse_port(port);
        if (parsed < 0) {
            SQLRETURN rc = fl_diag_setf(dbc, "HY000",
                "DSN attribute Port must be a number from 1 to 65535, got '%s'", port);
            free(port);
            free(database);
            free(schema);
            return rc;
        }
        dbc->port = parsed;
        free(port);
    }

    SQLRETURN rc = do_connect(dbc, database, schema);
    free(database);
    free(schema);
    return rc;
}

SQLRETURN SQL_API SQLDriverConnect(SQLHDBC ConnectionHandle, SQLHWND WindowHandle,
                                   SQLCHAR *InConnectionString, SQLSMALLINT StringLength1,
                                   SQLCHAR *OutConnectionString, SQLSMALLINT BufferLength,
                                   SQLSMALLINT *StringLength2Ptr, SQLUSMALLINT DriverCompletion) {
    (void) WindowHandle;
    (void) DriverCompletion;
    fl_dbc *dbc = (fl_dbc *) ConnectionHandle;
    if (dbc == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(dbc);
    if (dbc->connected) {
        return fl_diag_set(dbc, "08002", "Connection already open");
    }

    char conn[2048];
    size_t conn_length = StringLength1 == SQL_NTS
        ? strlen((const char *) InConnectionString)
        : (size_t) StringLength1;
    if (conn_length >= sizeof(conn)) {
        conn_length = sizeof(conn) - 1;
    }
    memcpy(conn, InConnectionString, conn_length);
    conn[conn_length] = '\0';

    /* connection-string keys override DSN attributes */
    char *dsn = conn_str_get(conn, "DSN");
    char *server = conn_str_get(conn, "Server");
    char *port = conn_str_get(conn, "Port");
    char *database = conn_str_get(conn, "Database");
    char *schema = conn_str_get(conn, "Schema");

    if (dsn != NULL) {
        if (server == NULL) {
            server = dsn_get(dsn, "Server");
        }
        if (port == NULL) {
            port = dsn_get(dsn, "Port");
        }
        if (database == NULL) {
            database = dsn_get(dsn, "Database");
        }
        if (schema == NULL) {
            schema = dsn_get(dsn, "Schema");
        }
        free(dbc->dsn);
        dbc->dsn = fl_strdup(dsn);
    }

    free(dbc->host);
    dbc->host = server != NULL ? server : fl_strdup("localhost");
    if (port != NULL) {
        int parsed = parse_port(port);
        if (parsed < 0) {
            SQLRETURN rc = fl_diag_setf(dbc, "HY000",
                "Port must be a number from 1 to 65535, got '%s'", port);
            free(dsn);
            free(port);
            free(database);
            free(schema);
            return rc;
        }
        dbc->port = parsed;
    }

    SQLRETURN rc = do_connect(dbc, database, schema);

    if (rc == SQL_SUCCESS && OutConnectionString != NULL) {
        char out[2048];
        snprintf(out, sizeof(out), "Driver={" FL_DRIVER_NAME "};Server=%s;Port=%d%s%s%s%s",
                 dbc->host, dbc->port,
                 database != NULL ? ";Database=" : "", database != NULL ? database : "",
                 schema != NULL ? ";Schema=" : "", schema != NULL ? schema : "");
        SQLSMALLINT out_length = 0;
        fl_copy_string(NULL, out, OutConnectionString, BufferLength, &out_length);
        if (StringLength2Ptr != NULL) {
            *StringLength2Ptr = out_length;
        }
    } else if (StringLength2Ptr != NULL) {
        *StringLength2Ptr = 0;
    }

    free(dsn);
    free(port);
    free(database);
    free(schema);
    return rc;
}

SQLRETURN SQL_API SQLDisconnect(SQLHDBC ConnectionHandle) {
    fl_dbc *dbc = (fl_dbc *) ConnectionHandle;
    if (dbc == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(dbc);
    if (!dbc->connected) {
        return fl_diag_set(dbc, "08003", "Connection not open");
    }
    dbc->connected = 0;
    free(dbc->session_id);
    dbc->session_id = NULL;
    return SQL_SUCCESS;
}

/* ---- transactions ---------------------------------------------------------- */

SQLRETURN SQL_API SQLEndTran(SQLSMALLINT HandleType, SQLHANDLE Handle, SQLSMALLINT CompletionType) {
    if (Handle == NULL) {
        return SQL_INVALID_HANDLE;
    }
    if (HandleType == SQL_HANDLE_ENV) {
        /* no per-environment connection registry: nothing to commit */
        return SQL_SUCCESS;
    }
    fl_dbc *dbc = (fl_dbc *) Handle;
    fl_diag_clear(dbc);
    if (!dbc->connected) {
        return fl_diag_set(dbc, "08003", "Connection not open");
    }
    const char *sql = CompletionType == SQL_COMMIT ? "COMMIT" : "ROLLBACK";
    char *transport_error = NULL;
    fl_response *response = fl_proto_execute(dbc, sql, &transport_error);
    if (response == NULL) {
        SQLRETURN rc = fl_diag_setf(dbc, "08S01", "%s", transport_error);
        free(transport_error);
        return rc;
    }
    if (response->error_message != NULL) {
        SQLRETURN rc = fl_diag_setf(dbc, "25000", "%s", response->error_message);
        fl_response_free(response);
        return rc;
    }
    fl_response_free(response);
    return SQL_SUCCESS;
}

/* ---- connection attributes ------------------------------------------------- */

SQLRETURN SQL_API SQLSetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute,
                                    SQLPOINTER ValuePtr, SQLINTEGER StringLength) {
    (void) StringLength;
    fl_dbc *dbc = (fl_dbc *) ConnectionHandle;
    if (dbc == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(dbc);
    switch (Attribute) {
        case SQL_ATTR_AUTOCOMMIT: {
            int wanted = ((SQLULEN) (uintptr_t) ValuePtr) == SQL_AUTOCOMMIT_ON;
            if (dbc->connected && wanted != dbc->autocommit) {
                /* mirrored to the engine the way the JDBC driver does it */
                char *transport_error = NULL;
                char sql[64];
                snprintf(sql, sizeof(sql), "ALTER SESSION SET AUTOCOMMIT = %s",
                         wanted ? "TRUE" : "FALSE");
                fl_response *response = fl_proto_execute(dbc, sql, &transport_error);
                if (response == NULL) {
                    SQLRETURN rc = fl_diag_setf(dbc, "08S01", "%s", transport_error);
                    free(transport_error);
                    return rc;
                }
                if (response->error_message != NULL) {
                    SQLRETURN rc = fl_diag_setf(dbc, "HY000", "%s", response->error_message);
                    fl_response_free(response);
                    return rc;
                }
                fl_response_free(response);
            }
            dbc->autocommit = wanted;
            return SQL_SUCCESS;
        }
        case SQL_ATTR_LOGIN_TIMEOUT:
        case SQL_ATTR_CONNECTION_TIMEOUT:
            dbc->login_timeout = (int) (SQLULEN) (uintptr_t) ValuePtr;
            return SQL_SUCCESS;
        case SQL_ATTR_CURRENT_CATALOG: {
            if (!dbc->connected) {
                free(dbc->database);
                dbc->database = fl_strdup((const char *) ValuePtr);
                return SQL_SUCCESS;
            }
            char sql[600];
            snprintf(sql, sizeof(sql), "USE DATABASE %s", (const char *) ValuePtr);
            char *transport_error = NULL;
            fl_response *response = fl_proto_execute(dbc, sql, &transport_error);
            if (response == NULL || response->error_message != NULL) {
                SQLRETURN rc = fl_diag_setf(dbc, "HY000", "%s",
                    response != NULL ? response->error_message : transport_error);
                free(transport_error);
                fl_response_free(response);
                return rc;
            }
            fl_response_free(response);
            free(dbc->database);
            dbc->database = fl_strdup((const char *) ValuePtr);
            return SQL_SUCCESS;
        }
        case SQL_ATTR_ACCESS_MODE:
        case SQL_ATTR_TXN_ISOLATION:
        case SQL_ATTR_ANSI_APP:
            return SQL_SUCCESS;
        default:
            /* tolerated: driver managers probe many optional attributes */
            return SQL_SUCCESS;
    }
}

SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC ConnectionHandle, SQLINTEGER Attribute,
                                    SQLPOINTER ValuePtr, SQLINTEGER BufferLength,
                                    SQLINTEGER *StringLengthPtr) {
    fl_dbc *dbc = (fl_dbc *) ConnectionHandle;
    if (dbc == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(dbc);
    switch (Attribute) {
        case SQL_ATTR_AUTOCOMMIT:
            if (ValuePtr != NULL) {
                *(SQLULEN *) ValuePtr = dbc->autocommit ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
            }
            return SQL_SUCCESS;
        case SQL_ATTR_CONNECTION_DEAD:
            if (ValuePtr != NULL) {
                *(SQLUINTEGER *) ValuePtr = dbc->connected ? SQL_CD_FALSE : SQL_CD_TRUE;
            }
            return SQL_SUCCESS;
        case SQL_ATTR_LOGIN_TIMEOUT:
        case SQL_ATTR_CONNECTION_TIMEOUT:
            if (ValuePtr != NULL) {
                *(SQLULEN *) ValuePtr = (SQLULEN) dbc->login_timeout;
            }
            return SQL_SUCCESS;
        case SQL_ATTR_CURRENT_CATALOG: {
            SQLSMALLINT out_length = 0;
            SQLRETURN rc = fl_copy_string(dbc, dbc->database != NULL ? dbc->database : "",
                                          (SQLCHAR *) ValuePtr, (SQLSMALLINT) BufferLength, &out_length);
            if (StringLengthPtr != NULL) {
                *StringLengthPtr = out_length;
            }
            return rc;
        }
        case SQL_ATTR_TXN_ISOLATION:
            if (ValuePtr != NULL) {
                *(SQLUINTEGER *) ValuePtr = SQL_TXN_READ_COMMITTED;
            }
            return SQL_SUCCESS;
        default:
            return fl_diag_set(dbc, "HY092", "Unsupported connection attribute");
    }
}

/* ---- SQLGetInfo ------------------------------------------------------------ */

typedef enum info_width { INFO_STRING, INFO_U16, INFO_U32 } info_width;

typedef struct info_entry {
    SQLUSMALLINT type;
    info_width width;
    const char *string_value;
    SQLUINTEGER numeric_value;
} info_entry;

SQLRETURN SQL_API SQLGetInfo(SQLHDBC ConnectionHandle, SQLUSMALLINT InfoType,
                             SQLPOINTER InfoValuePtr, SQLSMALLINT BufferLength,
                             SQLSMALLINT *StringLengthPtr) {
    fl_dbc *dbc = (fl_dbc *) ConnectionHandle;
    if (dbc == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(dbc);

    static const info_entry entries[] = {
        { SQL_DRIVER_NAME, INFO_STRING, "libfrostlakeodbc.so", 0 },
        { SQL_DRIVER_VER, INFO_STRING, FL_DRIVER_VER, 0 },
        { SQL_DRIVER_ODBC_VER, INFO_STRING, FL_ODBC_VER, 0 },
        { SQL_DBMS_NAME, INFO_STRING, FL_DBMS_NAME, 0 },
        { SQL_DBMS_VER, INFO_STRING, "01.00.0000", 0 },
        { SQL_IDENTIFIER_QUOTE_CHAR, INFO_STRING, "\"", 0 },
        { SQL_SEARCH_PATTERN_ESCAPE, INFO_STRING, "\\", 0 },
        { SQL_CATALOG_NAME_SEPARATOR, INFO_STRING, ".", 0 },
        { SQL_CATALOG_TERM, INFO_STRING, "database", 0 },
        { SQL_SCHEMA_TERM, INFO_STRING, "schema", 0 },
        { SQL_TABLE_TERM, INFO_STRING, "table", 0 },
        { SQL_PROCEDURE_TERM, INFO_STRING, "procedure", 0 },
        { SQL_CATALOG_NAME, INFO_STRING, "Y", 0 },
        { SQL_ACCESSIBLE_TABLES, INFO_STRING, "Y", 0 },
        { SQL_ACCESSIBLE_PROCEDURES, INFO_STRING, "Y", 0 },
        { SQL_COLUMN_ALIAS, INFO_STRING, "Y", 0 },
        { SQL_EXPRESSIONS_IN_ORDERBY, INFO_STRING, "Y", 0 },
        { SQL_LIKE_ESCAPE_CLAUSE, INFO_STRING, "Y", 0 },
        { SQL_ORDER_BY_COLUMNS_IN_SELECT, INFO_STRING, "N", 0 },
        { SQL_OUTER_JOINS, INFO_STRING, "Y", 0 },
        { SQL_PROCEDURES, INFO_STRING, "Y", 0 },
        { SQL_ROW_UPDATES, INFO_STRING, "N", 0 },
        { SQL_KEYWORDS, INFO_STRING, "", 0 },
        { SQL_SPECIAL_CHARACTERS, INFO_STRING, "", 0 },
        { SQL_MAX_ROW_SIZE_INCLUDES_LONG, INFO_STRING, "Y", 0 },
        { SQL_MULT_RESULT_SETS, INFO_STRING, "Y", 0 },
        { SQL_MULTIPLE_ACTIVE_TXN, INFO_STRING, "Y", 0 },
        { SQL_NEED_LONG_DATA_LEN, INFO_STRING, "N", 0 },
        { SQL_DESCRIBE_PARAMETER, INFO_STRING, "N", 0 },
        { SQL_DATA_SOURCE_READ_ONLY, INFO_STRING, "N", 0 },
        { SQL_DATABASE_NAME, INFO_STRING, NULL, 0 }, /* resolved below */
        { SQL_DATA_SOURCE_NAME, INFO_STRING, NULL, 0 },
        { SQL_SERVER_NAME, INFO_STRING, NULL, 0 },
        { SQL_USER_NAME, INFO_STRING, "", 0 },

        { SQL_TXN_CAPABLE, INFO_U16, NULL, SQL_TC_ALL },
        { SQL_CURSOR_COMMIT_BEHAVIOR, INFO_U16, NULL, SQL_CB_PRESERVE },
        { SQL_CURSOR_ROLLBACK_BEHAVIOR, INFO_U16, NULL, SQL_CB_PRESERVE },
        { SQL_CORRELATION_NAME, INFO_U16, NULL, SQL_CN_ANY },
        { SQL_NON_NULLABLE_COLUMNS, INFO_U16, NULL, SQL_NNC_NON_NULL },
        { SQL_GROUP_BY, INFO_U16, NULL, SQL_GB_GROUP_BY_CONTAINS_SELECT },
        { SQL_IDENTIFIER_CASE, INFO_U16, NULL, SQL_IC_UPPER },
        { SQL_QUOTED_IDENTIFIER_CASE, INFO_U16, NULL, SQL_IC_SENSITIVE },
        { SQL_CONCAT_NULL_BEHAVIOR, INFO_U16, NULL, SQL_CB_NULL },
        { SQL_NULL_COLLATION, INFO_U16, NULL, SQL_NC_HIGH },
        { SQL_ACTIVE_ENVIRONMENTS, INFO_U16, NULL, 0 },
        { SQL_MAX_CONCURRENT_ACTIVITIES, INFO_U16, NULL, 0 },
        { SQL_MAX_DRIVER_CONNECTIONS, INFO_U16, NULL, 0 },
        { SQL_MAX_COLUMN_NAME_LEN, INFO_U16, NULL, 255 },
        { SQL_MAX_TABLE_NAME_LEN, INFO_U16, NULL, 255 },
        { SQL_MAX_SCHEMA_NAME_LEN, INFO_U16, NULL, 255 },
        { SQL_MAX_CATALOG_NAME_LEN, INFO_U16, NULL, 255 },
        { SQL_MAX_IDENTIFIER_LEN, INFO_U16, NULL, 255 },
        { SQL_MAX_COLUMNS_IN_GROUP_BY, INFO_U16, NULL, 0 },
        { SQL_MAX_COLUMNS_IN_ORDER_BY, INFO_U16, NULL, 0 },
        { SQL_MAX_COLUMNS_IN_SELECT, INFO_U16, NULL, 0 },
        { SQL_MAX_COLUMNS_IN_TABLE, INFO_U16, NULL, 0 },
        { SQL_MAX_USER_NAME_LEN, INFO_U16, NULL, 0 },
        { SQL_FILE_USAGE, INFO_U16, NULL, SQL_FILE_NOT_SUPPORTED },

        { SQL_ODBC_INTERFACE_CONFORMANCE, INFO_U32, NULL, SQL_OIC_CORE },
        { SQL_SQL_CONFORMANCE, INFO_U32, NULL, SQL_SC_SQL92_ENTRY },
        { SQL_DEFAULT_TXN_ISOLATION, INFO_U32, NULL, SQL_TXN_READ_COMMITTED },
        { SQL_TXN_ISOLATION_OPTION, INFO_U32, NULL, SQL_TXN_READ_COMMITTED },
        { SQL_GETDATA_EXTENSIONS, INFO_U32, NULL,
          SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND },
        { SQL_SCHEMA_USAGE, INFO_U32, NULL,
          SQL_SU_DML_STATEMENTS | SQL_SU_TABLE_DEFINITION | SQL_SU_PRIVILEGE_DEFINITION },
        { SQL_CATALOG_USAGE, INFO_U32, NULL,
          SQL_CU_DML_STATEMENTS | SQL_CU_TABLE_DEFINITION },
        { SQL_OJ_CAPABILITIES, INFO_U32, NULL,
          SQL_OJ_LEFT | SQL_OJ_RIGHT | SQL_OJ_FULL | SQL_OJ_NESTED | SQL_OJ_ALL_COMPARISON_OPS },
        { SQL_SCROLL_OPTIONS, INFO_U32, NULL, SQL_SO_FORWARD_ONLY },
        { SQL_CURSOR_SENSITIVITY, INFO_U32, NULL, SQL_INSENSITIVE },
        { SQL_BOOKMARK_PERSISTENCE, INFO_U32, NULL, 0 },
        { SQL_MAX_ROW_SIZE, INFO_U32, NULL, 0 },
        { SQL_MAX_STATEMENT_LEN, INFO_U32, NULL, 0 },
        { SQL_MAX_BINARY_LITERAL_LEN, INFO_U32, NULL, 0 },
        { SQL_MAX_CHAR_LITERAL_LEN, INFO_U32, NULL, 0 },
        { SQL_MAX_INDEX_SIZE, INFO_U32, NULL, 0 },
        { SQL_MAX_TABLES_IN_SELECT, INFO_U16, NULL, 0 },
        { SQL_BATCH_ROW_COUNT, INFO_U32, NULL, 0 },
        { SQL_BATCH_SUPPORT, INFO_U32, NULL, 0 },
        { SQL_PARAM_ARRAY_ROW_COUNTS, INFO_U32, NULL, SQL_PARC_NO_BATCH },
        { SQL_PARAM_ARRAY_SELECTS, INFO_U32, NULL, SQL_PAS_NO_SELECT },
        { SQL_DTC_TRANSITION_COST, INFO_U32, NULL, 0 },
        { SQL_ASYNC_MODE, INFO_U32, NULL, SQL_AM_NONE },
        { SQL_ALTER_TABLE, INFO_U32, NULL,
          SQL_AT_ADD_COLUMN | SQL_AT_DROP_COLUMN },
        { SQL_UNION, INFO_U32, NULL, SQL_U_UNION | SQL_U_UNION_ALL },
        { SQL_SUBQUERIES, INFO_U32, NULL,
          SQL_SQ_COMPARISON | SQL_SQ_EXISTS | SQL_SQ_IN | SQL_SQ_QUANTIFIED | SQL_SQ_CORRELATED_SUBQUERIES },
        { SQL_AGGREGATE_FUNCTIONS, INFO_U32, NULL,
          SQL_AF_ALL | SQL_AF_AVG | SQL_AF_COUNT | SQL_AF_DISTINCT | SQL_AF_MAX | SQL_AF_MIN | SQL_AF_SUM },
        { SQL_NUMERIC_FUNCTIONS, INFO_U32, NULL, 0 },
        { SQL_STRING_FUNCTIONS, INFO_U32, NULL, 0 },
        { SQL_TIMEDATE_FUNCTIONS, INFO_U32, NULL, 0 },
        { SQL_SYSTEM_FUNCTIONS, INFO_U32, NULL, 0 },
        { SQL_CONVERT_FUNCTIONS, INFO_U32, NULL, 0 },
        { SQL_POS_OPERATIONS, INFO_U32, NULL, 0 },
        { SQL_STATIC_CURSOR_ATTRIBUTES1, INFO_U32, NULL, SQL_CA1_NEXT },
        { SQL_STATIC_CURSOR_ATTRIBUTES2, INFO_U32, NULL, 0 },
        { SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, INFO_U32, NULL, SQL_CA1_NEXT },
        { SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2, INFO_U32, NULL, 0 },
        { SQL_KEYSET_CURSOR_ATTRIBUTES1, INFO_U32, NULL, 0 },
        { SQL_KEYSET_CURSOR_ATTRIBUTES2, INFO_U32, NULL, 0 },
        { SQL_DYNAMIC_CURSOR_ATTRIBUTES1, INFO_U32, NULL, 0 },
        { SQL_DYNAMIC_CURSOR_ATTRIBUTES2, INFO_U32, NULL, 0 },
        { SQL_INFO_SCHEMA_VIEWS, INFO_U32, NULL, SQL_ISV_TABLES | SQL_ISV_COLUMNS | SQL_ISV_VIEWS },
        { SQL_SQL92_PREDICATES, INFO_U32, NULL, 0 },
        { SQL_SQL92_RELATIONAL_JOIN_OPERATORS, INFO_U32, NULL, 0 },
        { SQL_SQL92_VALUE_EXPRESSIONS, INFO_U32, NULL, 0 },
        { SQL_DATETIME_LITERALS, INFO_U32, NULL, 0 },
        { SQL_CONVERT_VARCHAR, INFO_U32, NULL, 0 },
        { SQL_CREATE_TABLE, INFO_U32, NULL, SQL_CT_CREATE_TABLE },
        { SQL_CREATE_VIEW, INFO_U32, NULL, SQL_CV_CREATE_VIEW },
        { SQL_DROP_TABLE, INFO_U32, NULL, SQL_DT_DROP_TABLE },
        { SQL_DROP_VIEW, INFO_U32, NULL, SQL_DV_DROP_VIEW },
        { SQL_INSERT_STATEMENT, INFO_U32, NULL,
          SQL_IS_INSERT_LITERALS | SQL_IS_INSERT_SEARCHED | SQL_IS_SELECT_INTO },
    };

    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (entries[i].type != InfoType) {
            continue;
        }
        if (entries[i].width == INFO_STRING) {
            const char *value = entries[i].string_value;
            if (value == NULL) {
                value = InfoType == SQL_DATABASE_NAME ? (dbc->database != NULL ? dbc->database : "")
                    : InfoType == SQL_DATA_SOURCE_NAME ? (dbc->dsn != NULL ? dbc->dsn : "")
                    : InfoType == SQL_SERVER_NAME ? (dbc->host != NULL ? dbc->host : "")
                    : "";
            }
            return fl_copy_string(dbc, value, (SQLCHAR *) InfoValuePtr, BufferLength, StringLengthPtr);
        }
        if (entries[i].width == INFO_U16) {
            if (InfoValuePtr != NULL) {
                *(SQLUSMALLINT *) InfoValuePtr = (SQLUSMALLINT) entries[i].numeric_value;
            }
            if (StringLengthPtr != NULL) {
                *StringLengthPtr = sizeof(SQLUSMALLINT);
            }
            return SQL_SUCCESS;
        }
        if (InfoValuePtr != NULL) {
            *(SQLUINTEGER *) InfoValuePtr = entries[i].numeric_value;
        }
        if (StringLengthPtr != NULL) {
            *StringLengthPtr = sizeof(SQLUINTEGER);
        }
        return SQL_SUCCESS;
    }
    return fl_diag_setf(dbc, "HY096", "Unsupported SQLGetInfo type %d", (int) InfoType);
}

SQLRETURN SQL_API SQLGetFunctions(SQLHDBC ConnectionHandle, SQLUSMALLINT FunctionId,
                                  SQLUSMALLINT *SupportedPtr) {
    fl_dbc *dbc = (fl_dbc *) ConnectionHandle;
    if (dbc == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(dbc);
    static const SQLUSMALLINT supported[] = {
        SQL_API_SQLALLOCHANDLE, SQL_API_SQLFREEHANDLE, SQL_API_SQLFREESTMT,
        SQL_API_SQLALLOCENV, SQL_API_SQLALLOCCONNECT, SQL_API_SQLALLOCSTMT,
        SQL_API_SQLCONNECT, SQL_API_SQLDRIVERCONNECT, SQL_API_SQLDISCONNECT,
        SQL_API_SQLGETINFO, SQL_API_SQLGETFUNCTIONS, SQL_API_SQLGETTYPEINFO,
        SQL_API_SQLSETENVATTR, SQL_API_SQLGETENVATTR,
        SQL_API_SQLSETCONNECTATTR, SQL_API_SQLGETCONNECTATTR,
        SQL_API_SQLSETSTMTATTR, SQL_API_SQLGETSTMTATTR,
        SQL_API_SQLEXECDIRECT, SQL_API_SQLPREPARE, SQL_API_SQLEXECUTE,
        SQL_API_SQLBINDPARAMETER, SQL_API_SQLNUMPARAMS,
        SQL_API_SQLNUMRESULTCOLS, SQL_API_SQLDESCRIBECOL, SQL_API_SQLCOLATTRIBUTE,
        SQL_API_SQLBINDCOL, SQL_API_SQLFETCH, SQL_API_SQLFETCHSCROLL,
        SQL_API_SQLGETDATA, SQL_API_SQLROWCOUNT, SQL_API_SQLMORERESULTS,
        SQL_API_SQLCLOSECURSOR, SQL_API_SQLCANCEL, SQL_API_SQLNATIVESQL,
        SQL_API_SQLENDTRAN, SQL_API_SQLGETDIAGREC, SQL_API_SQLGETDIAGFIELD,
        SQL_API_SQLTABLES, SQL_API_SQLCOLUMNS, SQL_API_SQLSTATISTICS,
        SQL_API_SQLSPECIALCOLUMNS, SQL_API_SQLPRIMARYKEYS,
    };
    if (FunctionId == SQL_API_ODBC3_ALL_FUNCTIONS) {
        memset(SupportedPtr, 0, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE * sizeof(SQLUSMALLINT));
        for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
            SQLUSMALLINT id = supported[i];
            SupportedPtr[id >> 4] |= (SQLUSMALLINT) (1 << (id & 0x000F));
        }
        return SQL_SUCCESS;
    }
    if (FunctionId == SQL_API_ALL_FUNCTIONS) {
        memset(SupportedPtr, 0, 100 * sizeof(SQLUSMALLINT));
        for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
            if (supported[i] < 100) {
                SupportedPtr[supported[i]] = SQL_TRUE;
            }
        }
        return SQL_SUCCESS;
    }
    *SupportedPtr = SQL_FALSE;
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (supported[i] == FunctionId) {
            *SupportedPtr = SQL_TRUE;
            break;
        }
    }
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLNativeSql(SQLHDBC ConnectionHandle, SQLCHAR *InStatementText,
                               SQLINTEGER TextLength1, SQLCHAR *OutStatementText,
                               SQLINTEGER BufferLength, SQLINTEGER *TextLength2Ptr) {
    fl_dbc *dbc = (fl_dbc *) ConnectionHandle;
    if (dbc == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(dbc);
    /* the driver performs no SQL rewriting */
    size_t length = TextLength1 == SQL_NTS ? strlen((const char *) InStatementText)
                                           : (size_t) TextLength1;
    if (TextLength2Ptr != NULL) {
        *TextLength2Ptr = (SQLINTEGER) length;
    }
    if (OutStatementText != NULL && BufferLength > 0) {
        size_t copy = length < (size_t) BufferLength - 1 ? length : (size_t) BufferLength - 1;
        memcpy(OutStatementText, InStatementText, copy);
        OutStatementText[copy] = '\0';
        if (copy < length) {
            fl_diag_set(dbc, "01004", "String data, right truncated");
            return SQL_SUCCESS_WITH_INFO;
        }
    }
    return SQL_SUCCESS;
}
