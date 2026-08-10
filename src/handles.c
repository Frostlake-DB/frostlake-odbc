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

/* Handle lifecycle and diagnostics: SQLAllocHandle / SQLFreeHandle /
 * SQLFreeStmt, environment attributes, SQLGetDiagRec / SQLGetDiagField. */

#include "fl_odbc.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- diagnostics ---------------------------------------------------------- */

void fl_diag_clear(SQLHANDLE handle) {
    if (handle != NULL) {
        ((fl_handle_header *) handle)->diag.present = 0;
    }
}

SQLRETURN fl_diag_set(SQLHANDLE handle, const char *sqlstate, const char *message) {
    if (handle == NULL) {
        return SQL_ERROR;
    }
    fl_diag *diag = &((fl_handle_header *) handle)->diag;
    snprintf(diag->sqlstate, sizeof(diag->sqlstate), "%s", sqlstate);
    snprintf(diag->message, sizeof(diag->message), "%s", message != NULL ? message : "");
    diag->native_error = 0;
    diag->present = 1;
    return SQL_ERROR;
}

SQLRETURN fl_diag_warn(SQLHANDLE handle, const char *sqlstate, const char *message) {
    fl_diag_set(handle, sqlstate, message);
    return handle == NULL ? SQL_ERROR : SQL_SUCCESS_WITH_INFO;
}

SQLRETURN fl_diag_setf(SQLHANDLE handle, const char *sqlstate, const char *fmt, ...) {
    char message[FL_MAX_DIAG_MSG];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    return fl_diag_set(handle, sqlstate, message);
}

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT HandleType, SQLHANDLE Handle,
                                SQLSMALLINT RecNumber, SQLCHAR *SQLState,
                                SQLINTEGER *NativeErrorPtr, SQLCHAR *MessageText,
                                SQLSMALLINT BufferLength, SQLSMALLINT *TextLengthPtr) {
    (void) HandleType;
    if (Handle == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag *diag = &((fl_handle_header *) Handle)->diag;
    if (RecNumber != 1 || !diag->present) {
        return SQL_NO_DATA;
    }
    if (SQLState != NULL) {
        memcpy(SQLState, diag->sqlstate, 6);
    }
    if (NativeErrorPtr != NULL) {
        *NativeErrorPtr = diag->native_error;
    }
    return fl_copy_string(NULL, diag->message, MessageText, BufferLength, TextLengthPtr);
}

SQLRETURN SQL_API SQLGetDiagField(SQLSMALLINT HandleType, SQLHANDLE Handle,
                                  SQLSMALLINT RecNumber, SQLSMALLINT DiagIdentifier,
                                  SQLPOINTER DiagInfoPtr, SQLSMALLINT BufferLength,
                                  SQLSMALLINT *StringLengthPtr) {
    (void) HandleType;
    if (Handle == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag *diag = &((fl_handle_header *) Handle)->diag;
    switch (DiagIdentifier) {
        case SQL_DIAG_NUMBER:
            if (DiagInfoPtr != NULL) {
                *(SQLINTEGER *) DiagInfoPtr = diag->present ? 1 : 0;
            }
            return SQL_SUCCESS;
        case SQL_DIAG_SQLSTATE:
            if (RecNumber != 1 || !diag->present) {
                return SQL_NO_DATA;
            }
            return fl_copy_string(NULL, diag->sqlstate, (SQLCHAR *) DiagInfoPtr,
                                  BufferLength, StringLengthPtr);
        case SQL_DIAG_MESSAGE_TEXT:
            if (RecNumber != 1 || !diag->present) {
                return SQL_NO_DATA;
            }
            return fl_copy_string(NULL, diag->message, (SQLCHAR *) DiagInfoPtr,
                                  BufferLength, StringLengthPtr);
        case SQL_DIAG_NATIVE:
            if (RecNumber != 1 || !diag->present) {
                return SQL_NO_DATA;
            }
            if (DiagInfoPtr != NULL) {
                *(SQLINTEGER *) DiagInfoPtr = diag->native_error;
            }
            return SQL_SUCCESS;
        default:
            return SQL_NO_DATA;
    }
}

/* ---- allocation ----------------------------------------------------------- */

static SQLRETURN alloc_env(SQLHANDLE *out) {
    fl_env *env = calloc(1, sizeof(fl_env));
    if (env == NULL) {
        return SQL_ERROR;
    }
    env->hdr.handle_type = SQL_HANDLE_ENV;
    env->odbc_version = SQL_OV_ODBC3;
    *out = env;
    return SQL_SUCCESS;
}

static SQLRETURN alloc_dbc(fl_env *env, SQLHANDLE *out) {
    fl_dbc *dbc = calloc(1, sizeof(fl_dbc));
    if (dbc == NULL) {
        return SQL_ERROR;
    }
    dbc->hdr.handle_type = SQL_HANDLE_DBC;
    dbc->env = env;
    dbc->autocommit = 1;
    dbc->port = 18082;
    /* Both default to something finite. They used to be 0 — "no timeout" — so a
     * server that accepted the socket and then went quiet hung the application
     * for ever, with no way to notice. Same shape as the Ruby driver's
     * open_timeout / read_timeout pair. */
    dbc->login_timeout = 10;
    dbc->request_timeout = 300;
    *out = dbc;
    return SQL_SUCCESS;
}

static SQLRETURN alloc_stmt(fl_dbc *dbc, SQLHANDLE *out) {
    if (dbc == NULL || !dbc->connected) {
        return fl_diag_set(dbc, "08003", "Connection not open");
    }
    fl_stmt *stmt = calloc(1, sizeof(fl_stmt));
    if (stmt == NULL) {
        return SQL_ERROR;
    }
    stmt->hdr.handle_type = SQL_HANDLE_STMT;
    stmt->dbc = dbc;
    stmt->current_row = -1;
    stmt->row_count = -1;
    stmt->getdata_col = -1;
    *out = stmt;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT HandleType, SQLHANDLE InputHandle,
                                 SQLHANDLE *OutputHandlePtr) {
    if (OutputHandlePtr == NULL) {
        return SQL_ERROR;
    }
    *OutputHandlePtr = SQL_NULL_HANDLE;
    switch (HandleType) {
        case SQL_HANDLE_ENV:
            return alloc_env(OutputHandlePtr);
        case SQL_HANDLE_DBC:
            if (InputHandle == NULL) {
                return SQL_INVALID_HANDLE;
            }
            fl_diag_clear(InputHandle);
            return alloc_dbc((fl_env *) InputHandle, OutputHandlePtr);
        case SQL_HANDLE_STMT:
            if (InputHandle == NULL) {
                return SQL_INVALID_HANDLE;
            }
            fl_diag_clear(InputHandle);
            return alloc_stmt((fl_dbc *) InputHandle, OutputHandlePtr);
        default:
            return SQL_ERROR;
    }
}

void fl_stmt_clear_results(fl_stmt *stmt) {
    fl_response_free(stmt->response);
    stmt->response = NULL;
    if (stmt->synth != NULL) {
        fl_resultset_free_contents(stmt->synth);
        free(stmt->synth);
        stmt->synth = NULL;
    }
    stmt->current_set = 0;
    stmt->current_row = -1;
    stmt->row_count = -1;
    stmt->getdata_col = -1;
    stmt->getdata_offset = 0;
}

static void free_stmt(fl_stmt *stmt) {
    fl_stmt_clear_results(stmt);
    free(stmt->prepared_sql);
    free(stmt->bound_cols);
    free(stmt->params);
    free(stmt);
}

static void free_dbc(fl_dbc *dbc) {
    free(dbc->host);
    free(dbc->session_id);
    free(dbc->database);
    free(dbc->schema);
    free(dbc->dsn);
    free(dbc);
}

SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT HandleType, SQLHANDLE Handle) {
    if (Handle == NULL) {
        return SQL_INVALID_HANDLE;
    }
    switch (HandleType) {
        case SQL_HANDLE_ENV:
            free(Handle);
            return SQL_SUCCESS;
        case SQL_HANDLE_DBC:
            free_dbc((fl_dbc *) Handle);
            return SQL_SUCCESS;
        case SQL_HANDLE_STMT:
            free_stmt((fl_stmt *) Handle);
            return SQL_SUCCESS;
        default:
            return SQL_ERROR;
    }
}

SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT StatementHandle, SQLUSMALLINT Option) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    switch (Option) {
        case SQL_CLOSE:
            fl_stmt_clear_results(stmt);
            return SQL_SUCCESS;
        case SQL_UNBIND:
            free(stmt->bound_cols);
            stmt->bound_cols = NULL;
            stmt->bound_col_capacity = 0;
            return SQL_SUCCESS;
        case SQL_RESET_PARAMS:
            free(stmt->params);
            stmt->params = NULL;
            stmt->param_capacity = 0;
            return SQL_SUCCESS;
        case SQL_DROP:
            free_stmt(stmt);
            return SQL_SUCCESS;
        default:
            return fl_diag_set(stmt, "HY092", "Invalid SQLFreeStmt option");
    }
}

/* ---- environment attributes ----------------------------------------------- */

SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute,
                                SQLPOINTER ValuePtr, SQLINTEGER StringLength) {
    (void) StringLength;
    fl_env *env = (fl_env *) EnvironmentHandle;
    if (env == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(env);
    switch (Attribute) {
        case SQL_ATTR_ODBC_VERSION:
            env->odbc_version = (SQLINTEGER) (intptr_t) ValuePtr;
            return SQL_SUCCESS;
        case SQL_ATTR_CONNECTION_POOLING:
        case SQL_ATTR_CP_MATCH:
        case SQL_ATTR_OUTPUT_NTS:
            return SQL_SUCCESS;
        default:
            return fl_diag_set(env, "HY092", "Unsupported environment attribute");
    }
}

SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV EnvironmentHandle, SQLINTEGER Attribute,
                                SQLPOINTER ValuePtr, SQLINTEGER BufferLength,
                                SQLINTEGER *StringLengthPtr) {
    (void) BufferLength;
    (void) StringLengthPtr;
    fl_env *env = (fl_env *) EnvironmentHandle;
    if (env == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(env);
    switch (Attribute) {
        case SQL_ATTR_ODBC_VERSION:
            if (ValuePtr != NULL) {
                *(SQLINTEGER *) ValuePtr = env->odbc_version;
            }
            return SQL_SUCCESS;
        case SQL_ATTR_OUTPUT_NTS:
            if (ValuePtr != NULL) {
                *(SQLINTEGER *) ValuePtr = SQL_TRUE;
            }
            return SQL_SUCCESS;
        default:
            return fl_diag_set(env, "HY092", "Unsupported environment attribute");
    }
}

/* Legacy ODBC 2.x allocation trio: the driver manager may still route these. */

SQLRETURN SQL_API SQLAllocEnv(SQLHENV *EnvironmentHandle) {
    return SQLAllocHandle(SQL_HANDLE_ENV, NULL, (SQLHANDLE *) EnvironmentHandle);
}

SQLRETURN SQL_API SQLAllocConnect(SQLHENV EnvironmentHandle, SQLHDBC *ConnectionHandle) {
    return SQLAllocHandle(SQL_HANDLE_DBC, EnvironmentHandle, (SQLHANDLE *) ConnectionHandle);
}

SQLRETURN SQL_API SQLAllocStmt(SQLHDBC ConnectionHandle, SQLHSTMT *StatementHandle) {
    return SQLAllocHandle(SQL_HANDLE_STMT, ConnectionHandle, (SQLHANDLE *) StatementHandle);
}

SQLRETURN SQL_API SQLFreeEnv(SQLHENV EnvironmentHandle) {
    return SQLFreeHandle(SQL_HANDLE_ENV, EnvironmentHandle);
}

SQLRETURN SQL_API SQLFreeConnect(SQLHDBC ConnectionHandle) {
    return SQLFreeHandle(SQL_HANDLE_DBC, ConnectionHandle);
}

/* ODBC 2.x diagnostics: some tools still call SQLError. */
SQLRETURN SQL_API SQLError(SQLHENV EnvironmentHandle, SQLHDBC ConnectionHandle,
                           SQLHSTMT StatementHandle, SQLCHAR *SQLState,
                           SQLINTEGER *NativeErrorPtr, SQLCHAR *MessageText,
                           SQLSMALLINT BufferLength, SQLSMALLINT *TextLengthPtr) {
    SQLHANDLE handle = StatementHandle != NULL ? StatementHandle
        : ConnectionHandle != NULL ? (SQLHANDLE) ConnectionHandle
        : (SQLHANDLE) EnvironmentHandle;
    if (handle == NULL) {
        return SQL_INVALID_HANDLE;
    }
    SQLRETURN rc = SQLGetDiagRec(0, handle, 1, SQLState, NativeErrorPtr,
                                 MessageText, BufferLength, TextLengthPtr);
    /* SQLError consumes the record */
    if (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) {
        ((fl_handle_header *) handle)->diag.present = 0;
    }
    return rc;
}
