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
 * The driver on its own, loaded with dlopen and called directly — no driver
 * manager anywhere. unixODBC tracks cursor state itself and answers some
 * misuse before the driver ever sees it, so a driver-side guard can be wrong
 * and still look right through isql. These checks are the ones that only fail
 * without a manager in the way.
 *
 *   build/direct <path-to-libfrostlakeodbc.so>
 * with FROSTLAKE_TEST_PORT naming a live server.
 */

#include <sql.h>
#include <sqlext.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;

static void require(int condition, const char *what) {
    checks++;
    if (condition) {
        printf("ok %d - %s\n", checks, what);
        return;
    }
    fprintf(stderr, "FAILED at check %d: %s\n", checks, what);
    exit(1);
}

typedef SQLRETURN (*fn_alloc)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
typedef SQLRETURN (*fn_envattr)(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER);
typedef SQLRETURN (*fn_drvconn)(SQLHDBC, SQLHWND, SQLCHAR *, SQLSMALLINT, SQLCHAR *,
                                SQLSMALLINT, SQLSMALLINT *, SQLUSMALLINT);
typedef SQLRETURN (*fn_exec)(SQLHSTMT, SQLCHAR *, SQLINTEGER);
typedef SQLRETURN (*fn_fetch)(SQLHSTMT);
typedef SQLRETURN (*fn_getdata)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN *);
typedef SQLRETURN (*fn_setstmt)(SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER);
typedef SQLRETURN (*fn_free)(SQLSMALLINT, SQLHANDLE);

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: direct <driver.so>\n");
        return 2;
    }
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (lib == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }
    fn_alloc Alloc = (fn_alloc) dlsym(lib, "SQLAllocHandle");
    fn_envattr EnvAttr = (fn_envattr) dlsym(lib, "SQLSetEnvAttr");
    fn_drvconn Connect = (fn_drvconn) dlsym(lib, "SQLDriverConnect");
    fn_exec Exec = (fn_exec) dlsym(lib, "SQLExecDirect");
    fn_fetch Fetch = (fn_fetch) dlsym(lib, "SQLFetch");
    fn_getdata GetData = (fn_getdata) dlsym(lib, "SQLGetData");
    fn_setstmt SetStmt = (fn_setstmt) dlsym(lib, "SQLSetStmtAttr");
    fn_free Free = (fn_free) dlsym(lib, "SQLFreeHandle");
    require(Alloc && EnvAttr && Connect && Exec && Fetch && GetData && SetStmt && Free,
            "the driver exports the ODBC entry points");

    const char *port = getenv("FROSTLAKE_TEST_PORT");
    char conn[256];
    snprintf(conn, sizeof(conn), "Server=localhost;Port=%s", port != NULL ? port : "18095");

    SQLHENV env = NULL;
    SQLHDBC dbc = NULL;
    SQLHSTMT stmt = NULL;
    SQLCHAR out[512];
    SQLSMALLINT out_length = 0;
    Alloc(SQL_HANDLE_ENV, NULL, &env);
    EnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER) SQL_OV_ODBC3, 0);
    Alloc(SQL_HANDLE_DBC, env, &dbc);
    require(SQL_SUCCEEDED(Connect(dbc, NULL, (SQLCHAR *) conn, SQL_NTS, out, sizeof(out),
                                  &out_length, SQL_DRIVER_NOPROMPT)),
            "connect straight to the driver");

    Alloc(SQL_HANDLE_STMT, dbc, &stmt);
    Exec(stmt, (SQLCHAR *) "CREATE OR REPLACE DATABASE odbc_direct", SQL_NTS);
    Exec(stmt, (SQLCHAR *) "USE DATABASE odbc_direct", SQL_NTS);
    Exec(stmt, (SQLCHAR *) "USE SCHEMA public", SQL_NTS);
    Exec(stmt, (SQLCHAR *) "CREATE OR REPLACE TABLE t (id INT)", SQL_NTS);
    Exec(stmt, (SQLCHAR *) "INSERT INTO t VALUES (10),(20),(30),(40),(50)", SQL_NTS);

    /* A cursor stopped early by SQL_ATTR_MAX_ROWS must be as finished as one
     * that ran out of rows: parking on the limit instead of past the result set
     * let the next SQLGetData return a row the caller was never given. */
    SQLHSTMT limited = NULL;
    Alloc(SQL_HANDLE_STMT, dbc, &limited);
    require(SQL_SUCCEEDED(SetStmt(limited, SQL_ATTR_MAX_ROWS, (SQLPOINTER) 2, 0)),
            "set SQL_ATTR_MAX_ROWS on the driver directly");
    require(SQL_SUCCEEDED(Exec(limited, (SQLCHAR *) "SELECT id FROM t ORDER BY id", SQL_NTS)),
            "exec under MAX_ROWS");
    int rows = 0;
    SQLBIGINT value = 0;
    SQLLEN indicator = 0;
    while (Fetch(limited) == SQL_SUCCESS) {
        GetData(limited, 1, SQL_C_SBIGINT, &value, 0, &indicator);
        rows++;
    }
    require(rows == 2, "MAX_ROWS=2 yields exactly two rows");
    require(value == 20, "the last row delivered is the second one");
    value = -1;
    require(GetData(limited, 1, SQL_C_SBIGINT, &value, 0, &indicator) == SQL_ERROR,
            "SQLGetData past a MAX_ROWS-exhausted cursor is refused");
    require(value == -1, "and it writes nothing into the caller's buffer");
    Free(SQL_HANDLE_STMT, limited);

    /* The same guard with no row limit in play. */
    SQLHSTMT plain = NULL;
    Alloc(SQL_HANDLE_STMT, dbc, &plain);
    Exec(plain, (SQLCHAR *) "SELECT id FROM t ORDER BY id", SQL_NTS);
    while (Fetch(plain) == SQL_SUCCESS) {
        /* drain */
    }
    require(GetData(plain, 1, SQL_C_SBIGINT, &value, 0, &indicator) == SQL_ERROR,
            "SQLGetData past an exhausted cursor is refused");
    Free(SQL_HANDLE_STMT, plain);

    Exec(stmt, (SQLCHAR *) "DROP DATABASE odbc_direct", SQL_NTS);
    Free(SQL_HANDLE_STMT, stmt);
    Free(SQL_HANDLE_DBC, dbc);
    Free(SQL_HANDLE_ENV, env);
    printf("all %d direct checks passed\n", checks);
    return 0;
}
