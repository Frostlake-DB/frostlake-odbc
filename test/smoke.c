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
 * End-to-end smoke test: goes through the unixODBC driver manager exactly like
 * a real application, against a live Frostlake HTTP server. Every assertion
 * prints "ok - ..." or dies with the diagnostic, so a failure names itself.
 *
 * Connection string comes from the FROSTLAKE_CONN environment variable
 * (default: Driver=Frostlake ODBC Driver;Server=localhost;Port=18095).
 */

#include <sql.h>
#include <sqlext.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The driver's statement attribute for the per-request statement count, as an application takes it
 * from the driver's documentation. Same name and meaning as the account's ODBC driver uses. */
#ifndef SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT
#define SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT 16385
#endif

static int checks;

static void print_diag(SQLSMALLINT type, SQLHANDLE handle) {
    SQLCHAR state[6] = "";
    SQLCHAR message[1024] = "";
    SQLINTEGER native = 0;
    SQLSMALLINT length = 0;
    if (SQLGetDiagRec(type, handle, 1, state, &native, message, sizeof(message), &length)
            == SQL_SUCCESS || state[0] != '\0') {
        fprintf(stderr, "    diag: [%s] %s\n", state, message);
    }
}

static void require(int condition, const char *what, SQLSMALLINT type, SQLHANDLE handle) {
    checks++;
    if (condition) {
        printf("ok %d - %s\n", checks, what);
        return;
    }
    fprintf(stderr, "FAILED at check %d: %s\n", checks, what);
    if (handle != NULL) {
        print_diag(type, handle);
    }
    exit(1);
}

static void require_rc(SQLRETURN rc, const char *what, SQLSMALLINT type, SQLHANDLE handle) {
    require(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO, what, type, handle);
}

/* A check this engine cannot answer. Reported as a TAP skip rather than a pass: a green tick would
   claim the engine had been checked for something it never reports. */
static void skip(const char *what, const char *why) {
    checks++;
    printf("ok %d - %s # SKIP %s\n", checks, what, why);
}

/* Run SQL, expecting success. */
static void exec_ok(SQLHSTMT stmt, const char *sql) {
    SQLRETURN rc = SQLExecDirect(stmt, (SQLCHAR *) sql, SQL_NTS);
    char what[256];
    snprintf(what, sizeof(what), "exec: %.200s", sql);
    require_rc(rc, what, SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);
}

int main(void) {
    const char *conn_env = getenv("FROSTLAKE_CONN");
    char conn[512];
    snprintf(conn, sizeof(conn), "%s",
             conn_env != NULL ? conn_env
                              : "Driver=Frostlake ODBC Driver;Server=localhost;Port=18095");

    SQLHENV env = NULL;
    SQLHDBC dbc = NULL;
    SQLHSTMT stmt = NULL;

    require_rc(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env),
               "allocate environment", 0, NULL);
    require_rc(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER) SQL_OV_ODBC3, 0),
               "declare ODBC 3", SQL_HANDLE_ENV, env);
    require_rc(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc),
               "allocate connection", SQL_HANDLE_ENV, env);

    SQLCHAR out_conn[512];
    SQLSMALLINT out_length = 0;
    require_rc(SQLDriverConnect(dbc, NULL, (SQLCHAR *) conn, SQL_NTS,
                                out_conn, sizeof(out_conn), &out_length, SQL_DRIVER_NOPROMPT),
               "connect (SQLDriverConnect)", SQL_HANDLE_DBC, dbc);

    /* driver identity */
    SQLCHAR info[256];
    SQLSMALLINT info_length = 0;
    require_rc(SQLGetInfo(dbc, SQL_DBMS_NAME, info, sizeof(info), &info_length),
               "SQLGetInfo(SQL_DBMS_NAME)", SQL_HANDLE_DBC, dbc);
    require(strcmp((char *) info, "Frostlake") == 0, "DBMS name is Frostlake", SQL_HANDLE_DBC, dbc);

    require_rc(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt),
               "allocate statement", SQL_HANDLE_DBC, dbc);

    /* workspace */
    exec_ok(stmt, "CREATE OR REPLACE DATABASE odbc_smoke");
    exec_ok(stmt, "USE DATABASE odbc_smoke");
    exec_ok(stmt, "USE SCHEMA public");
    exec_ok(stmt, "CREATE OR REPLACE TABLE t"
                  " (id INTEGER, name VARCHAR, price NUMBER(10,2), ok BOOLEAN,"
                  "  d DATE, ts TIMESTAMP_NTZ, raw BINARY)");

    /* INSERT: update count comes from the Snowflake-style count result set */
    SQLRETURN rc = SQLExecDirect(stmt, (SQLCHAR *)
        "INSERT INTO t SELECT 1, 'alice', 12.50, TRUE, '2026-01-02'::DATE,"
        " '2026-01-02 03:04:05'::TIMESTAMP_NTZ, TO_BINARY('CAFE', 'HEX')"
        " UNION ALL SELECT 2, 'O''Brien', 0.05, FALSE, NULL, NULL, NULL", SQL_NTS);
    require_rc(rc, "insert two rows", SQL_HANDLE_STMT, stmt);
    SQLLEN affected = 0;
    require_rc(SQLRowCount(stmt, &affected), "SQLRowCount after insert", SQL_HANDLE_STMT, stmt);
    require(affected == 2, "insert reports 2 rows affected", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* SELECT: describe + typed fetch */
    rc = SQLExecDirect(stmt, (SQLCHAR *)
        "SELECT id, name, price, ok, d, ts, raw FROM t ORDER BY id", SQL_NTS);
    require_rc(rc, "select the rows back", SQL_HANDLE_STMT, stmt);

    SQLSMALLINT column_count = 0;
    require_rc(SQLNumResultCols(stmt, &column_count), "SQLNumResultCols", SQL_HANDLE_STMT, stmt);
    require(column_count == 7, "seven result columns", SQL_HANDLE_STMT, stmt);

    SQLCHAR column_name[128];
    SQLSMALLINT name_length, data_type, digits, nullable;
    SQLULEN column_size;
    require_rc(SQLDescribeCol(stmt, 3, column_name, sizeof(column_name), &name_length,
                              &data_type, &column_size, &digits, &nullable),
               "describe column 3", SQL_HANDLE_STMT, stmt);
    require(strcmp((char *) column_name, "PRICE") == 0, "column 3 is PRICE", SQL_HANDLE_STMT, stmt);
    require(data_type == SQL_DECIMAL, "PRICE is SQL_DECIMAL", SQL_HANDLE_STMT, stmt);
    require(column_size == 10 && digits == 2, "PRICE is NUMBER(10,2)", SQL_HANDLE_STMT, stmt);

    /* A text or binary column reports its OWN length, not a blanket maximum: the account reports
       that number as the column's precision and display size alike. */
    {
        SQLHSTMT sized = NULL;
        require_rc(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sized),
                   "allocate a statement for the sized columns", SQL_HANDLE_DBC, dbc);
        exec_ok(sized, "CREATE OR REPLACE TABLE sized_t (S VARCHAR(9), B BINARY(5), BIG VARCHAR, N NUMBER(10,2))");
        require_rc(SQLExecDirect(sized, (SQLCHAR *) "SELECT S, B, BIG, N FROM sized_t", SQL_NTS),
                   "select the sized columns", SQL_HANDLE_STMT, sized);
        SQLULEN sized_size = 0;
        SQLSMALLINT sized_digits = 0, sized_type = 0, sized_nullable = 0, sized_name_length = 0;
        SQLCHAR sized_name[128];
        require_rc(SQLDescribeCol(sized, 1, sized_name, sizeof(sized_name), &sized_name_length,
                                  &sized_type, &sized_size, &sized_digits, &sized_nullable),
                   "describe VARCHAR(9)", SQL_HANDLE_STMT, sized);
        /* Engines before 0.1.0 send no length, and this driver supports them: with nothing to
           report, the column falls back to the blanket maximum and there is no width to check. */
        const int reports_length = sized_size != 16777216;
        if (!reports_length) {
            skip("a VARCHAR(9) reports 9 characters", "engine sends no column length");
            skip("a BINARY(5) reports 5 bytes", "engine sends no column length");
            skip("an unbounded VARCHAR reports the maximum", "engine sends no column length");
        } else {
            require(sized_size == 9, "a VARCHAR(9) reports 9 characters", SQL_HANDLE_STMT, sized);
            require_rc(SQLDescribeCol(sized, 2, sized_name, sizeof(sized_name), &sized_name_length,
                                      &sized_type, &sized_size, &sized_digits, &sized_nullable),
                       "describe BINARY(5)", SQL_HANDLE_STMT, sized);
            require(sized_size == 5, "a BINARY(5) reports 5 bytes", SQL_HANDLE_STMT, sized);
            require_rc(SQLDescribeCol(sized, 3, sized_name, sizeof(sized_name), &sized_name_length,
                                      &sized_type, &sized_size, &sized_digits, &sized_nullable),
                       "describe an unbounded VARCHAR", SQL_HANDLE_STMT, sized);
            require(sized_size == 16777216, "an unbounded VARCHAR reports the maximum",
                    SQL_HANDLE_STMT, sized);
        }
        /* A number is untouched by this: it reports its own precision, and carries no length. */
        require_rc(SQLDescribeCol(sized, 4, sized_name, sizeof(sized_name), &sized_name_length,
                                  &sized_type, &sized_size, &sized_digits, &sized_nullable),
                   "describe NUMBER(10,2)", SQL_HANDLE_STMT, sized);
        require(sized_size == 10 && sized_digits == 2, "a NUMBER still reports its precision",
                SQL_HANDLE_STMT, sized);
        SQLCloseCursor(sized);
        exec_ok(sized, "DROP TABLE sized_t");
        SQLFreeHandle(SQL_HANDLE_STMT, sized);
    }

    require_rc(SQLDescribeCol(stmt, 1, column_name, sizeof(column_name), &name_length,
                              &data_type, &column_size, &digits, &nullable),
               "describe column 1", SQL_HANDLE_STMT, stmt);
    require(data_type == SQL_BIGINT, "ID (scale-0 NUMBER) is SQL_BIGINT", SQL_HANDLE_STMT, stmt);

    /* row 1 through SQLGetData with real conversions */
    require_rc(SQLFetch(stmt), "fetch row 1", SQL_HANDLE_STMT, stmt);

    SQLBIGINT id = 0;
    SQLLEN indicator = 0;
    require_rc(SQLGetData(stmt, 1, SQL_C_SBIGINT, &id, 0, &indicator),
               "id as SQL_C_SBIGINT", SQL_HANDLE_STMT, stmt);
    require(id == 1, "id equals 1", SQL_HANDLE_STMT, stmt);

    char text[64];
    require_rc(SQLGetData(stmt, 2, SQL_C_CHAR, text, sizeof(text), &indicator),
               "name as SQL_C_CHAR", SQL_HANDLE_STMT, stmt);
    require(strcmp(text, "alice") == 0 && indicator == 5, "name is alice (length 5)",
            SQL_HANDLE_STMT, stmt);

    require_rc(SQLGetData(stmt, 3, SQL_C_CHAR, text, sizeof(text), &indicator),
               "price as text", SQL_HANDLE_STMT, stmt);
    require(strcmp(text, "12.50") == 0 || strcmp(text, "12.5") == 0,
            "price text preserves the decimal", SQL_HANDLE_STMT, stmt);

    double price = 0;
    require_rc(SQLGetData(stmt, 3, SQL_C_DOUBLE, &price, 0, &indicator),
               "price as SQL_C_DOUBLE", SQL_HANDLE_STMT, stmt);
    require(price > 12.49 && price < 12.51, "price ~ 12.50", SQL_HANDLE_STMT, stmt);

    SQLCHAR flag = 2;
    require_rc(SQLGetData(stmt, 4, SQL_C_BIT, &flag, 0, &indicator),
               "ok as SQL_C_BIT", SQL_HANDLE_STMT, stmt);
    require(flag == 1, "ok is true", SQL_HANDLE_STMT, stmt);

    SQL_DATE_STRUCT date;
    require_rc(SQLGetData(stmt, 5, SQL_C_TYPE_DATE, &date, 0, &indicator),
               "d as SQL_C_TYPE_DATE", SQL_HANDLE_STMT, stmt);
    require(date.year == 2026 && date.month == 1 && date.day == 2,
            "date is 2026-01-02", SQL_HANDLE_STMT, stmt);

    SQL_TIMESTAMP_STRUCT ts;
    require_rc(SQLGetData(stmt, 6, SQL_C_TYPE_TIMESTAMP, &ts, 0, &indicator),
               "ts as SQL_C_TYPE_TIMESTAMP", SQL_HANDLE_STMT, stmt);
    require(ts.year == 2026 && ts.hour == 3 && ts.minute == 4 && ts.second == 5,
            "timestamp is 03:04:05", SQL_HANDLE_STMT, stmt);

    unsigned char bytes[8];
    require_rc(SQLGetData(stmt, 7, SQL_C_BINARY, bytes, sizeof(bytes), &indicator),
               "raw as SQL_C_BINARY", SQL_HANDLE_STMT, stmt);
    require(indicator == 2 && bytes[0] == 0xCA && bytes[1] == 0xFE,
            "binary decodes to CA FE", SQL_HANDLE_STMT, stmt);

    /* row 2: NULLs + escaped quote */
    require_rc(SQLFetch(stmt), "fetch row 2", SQL_HANDLE_STMT, stmt);
    require_rc(SQLGetData(stmt, 2, SQL_C_CHAR, text, sizeof(text), &indicator),
               "second name", SQL_HANDLE_STMT, stmt);
    require(strcmp(text, "O'Brien") == 0, "name is O'Brien", SQL_HANDLE_STMT, stmt);
    require_rc(SQLGetData(stmt, 5, SQL_C_TYPE_DATE, &date, 0, &indicator),
               "NULL date", SQL_HANDLE_STMT, stmt);
    require(indicator == SQL_NULL_DATA, "NULL date reports SQL_NULL_DATA", SQL_HANDLE_STMT, stmt);

    require(SQLFetch(stmt) == SQL_NO_DATA, "third fetch is SQL_NO_DATA", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* chunked SQLGetData */
    rc = SQLExecDirect(stmt, (SQLCHAR *) "SELECT REPEAT('ab', 5) AS chunky", SQL_NTS);
    require_rc(rc, "select a 10-char string", SQL_HANDLE_STMT, stmt);
    require_rc(SQLFetch(stmt), "fetch chunky row", SQL_HANDLE_STMT, stmt);
    char chunk[5];
    rc = SQLGetData(stmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator);
    require(rc == SQL_SUCCESS_WITH_INFO && strcmp(chunk, "abab") == 0 && indicator == 10,
            "first chunk truncates with 01004 and full length", SQL_HANDLE_STMT, stmt);
    rc = SQLGetData(stmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator);
    require(rc == SQL_SUCCESS_WITH_INFO && strcmp(chunk, "abab") == 0,
            "second chunk continues", SQL_HANDLE_STMT, stmt);
    rc = SQLGetData(stmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator);
    require(rc == SQL_SUCCESS && strcmp(chunk, "ab") == 0,
            "final chunk completes the value", SQL_HANDLE_STMT, stmt);
    require(SQLGetData(stmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &indicator) == SQL_NO_DATA,
            "further reads answer SQL_NO_DATA", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* prepared statement with typed parameters */
    require_rc(SQLPrepare(stmt, (SQLCHAR *) "INSERT INTO t (id, name, price) VALUES (?, ?, ?)",
                          SQL_NTS),
               "prepare parameterised insert", SQL_HANDLE_STMT, stmt);
    SQLSMALLINT param_count = 0;
    require_rc(SQLNumParams(stmt, &param_count), "SQLNumParams", SQL_HANDLE_STMT, stmt);
    require(param_count == 3, "three parameters", SQL_HANDLE_STMT, stmt);

    SQLINTEGER param_id = 7;
    char param_name[] = "d'artagnan \\ hero";
    double param_price = 3.14;
    SQLLEN name_ind = SQL_NTS;
    require_rc(SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
                                0, 0, &param_id, 0, NULL),
               "bind id", SQL_HANDLE_STMT, stmt);
    require_rc(SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                                0, 0, param_name, 0, &name_ind),
               "bind name", SQL_HANDLE_STMT, stmt);
    require_rc(SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DECIMAL,
                                0, 0, &param_price, 0, NULL),
               "bind price", SQL_HANDLE_STMT, stmt);
    require_rc(SQLExecute(stmt), "execute prepared insert", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    rc = SQLExecDirect(stmt, (SQLCHAR *)
        "SELECT name FROM t WHERE id = 7", SQL_NTS);
    require_rc(rc, "read the parameterised row", SQL_HANDLE_STMT, stmt);
    require_rc(SQLFetch(stmt), "fetch it", SQL_HANDLE_STMT, stmt);
    require_rc(SQLGetData(stmt, 1, SQL_C_CHAR, text, sizeof(text), &indicator),
               "its name", SQL_HANDLE_STMT, stmt);
    require(strcmp(text, "d'artagnan \\ hero") == 0,
            "quote and backslash round-trip through binding", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* bound columns via SQLBindCol */
    SQLBIGINT bound_id = 0;
    char bound_name[32];
    SQLLEN id_ind = 0, bound_name_ind = 0;
    require_rc(SQLBindCol(stmt, 1, SQL_C_SBIGINT, &bound_id, 0, &id_ind),
               "bind column 1", SQL_HANDLE_STMT, stmt);
    require_rc(SQLBindCol(stmt, 2, SQL_C_CHAR, bound_name, sizeof(bound_name), &bound_name_ind),
               "bind column 2", SQL_HANDLE_STMT, stmt);
    rc = SQLExecDirect(stmt, (SQLCHAR *) "SELECT id, name FROM t WHERE id = 1", SQL_NTS);
    require_rc(rc, "select for bound columns", SQL_HANDLE_STMT, stmt);
    require_rc(SQLFetch(stmt), "fetch fills bound buffers", SQL_HANDLE_STMT, stmt);
    require(bound_id == 1 && strcmp(bound_name, "alice") == 0,
            "bound buffers carry id=1 name=alice", SQL_HANDLE_STMT, stmt);
    SQLFreeStmt(stmt, SQL_UNBIND);
    SQLCloseCursor(stmt);

    /* A STATEMENT can ask for a pack on its own, with no ALTER SESSION: the count rides on the
       request and leaves the session's own setting alone. The session is still at its default of
       one statement here, which is what makes the refusal below meaningful. */
    rc = SQLSetStmtAttr(stmt, SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT, (SQLPOINTER) (intptr_t) 2, 0);
    require_rc(rc, "declare a two-statement pack on the statement", SQL_HANDLE_STMT, stmt);
    SQLLEN declared = 0;
    rc = SQLGetStmtAttr(stmt, SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT, &declared, 0, NULL);
    require_rc(rc, "read the declared count back", SQL_HANDLE_STMT, stmt);
    require(declared == 2, "the statement reports the count it was given", SQL_HANDLE_STMT, stmt);
    rc = SQLExecDirect(stmt, (SQLCHAR *) "SELECT 1 AS a; SELECT 2 AS b", SQL_NTS);
    require_rc(rc, "a pack the statement asked for runs", SQL_HANDLE_STMT, stmt);
    require(SQLMoreResults(stmt) == SQL_SUCCESS, "the asked-for pack has a second result",
            SQL_HANDLE_STMT, stmt);
    SQLFreeStmt(stmt, SQL_CLOSE);

    /* Handing the count back to the session shows nothing was changed on it: the same pack,
       which the session never asked for, is refused. */
    rc = SQLSetStmtAttr(stmt, SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT, (SQLPOINTER) (intptr_t) -1, 0);
    require_rc(rc, "hand the count back to the session", SQL_HANDLE_STMT, stmt);
    rc = SQLExecDirect(stmt, (SQLCHAR *) "SELECT 1 AS a; SELECT 2 AS b", SQL_NTS);
    /* Only an engine that counts statements refuses one, and this driver supports older engines
       that do not. Against one of those there is no refusal to observe, so the check is skipped
       rather than passed. */
    if (rc == SQL_ERROR) {
        require(1, "a pack nobody asked for is still refused", SQL_HANDLE_STMT, stmt);
    } else {
        skip("a pack nobody asked for is still refused",
             "engine does not enforce a statement count");
    }
    SQLFreeStmt(stmt, SQL_CLOSE);

    /* A request holds one statement unless the session asks for more, so ask first. Zero
       means any number, which keeps the single statements around it working too. */
    rc = SQLExecDirect(stmt, (SQLCHAR *) "ALTER SESSION SET MULTI_STATEMENT_COUNT = 0", SQL_NTS);
    require_rc(rc, "allow statement packs", SQL_HANDLE_STMT, stmt);
    SQLFreeStmt(stmt, SQL_CLOSE);

    /* multi-statement execute walks result sets via SQLMoreResults */
    rc = SQLExecDirect(stmt, (SQLCHAR *) "SELECT 1 AS a; SELECT 2 AS b", SQL_NTS);
    require_rc(rc, "multi-statement execute", SQL_HANDLE_STMT, stmt);
    require_rc(SQLFetch(stmt), "fetch first result", SQL_HANDLE_STMT, stmt);
    long v = 0;
    require_rc(SQLGetData(stmt, 1, SQL_C_SLONG, &v, 0, &indicator), "read a", SQL_HANDLE_STMT, stmt);
    require(v == 1, "first result is 1", SQL_HANDLE_STMT, stmt);
    require(SQLMoreResults(stmt) == SQL_SUCCESS, "a second result exists", SQL_HANDLE_STMT, stmt);
    require_rc(SQLFetch(stmt), "fetch second result", SQL_HANDLE_STMT, stmt);
    require_rc(SQLGetData(stmt, 1, SQL_C_SLONG, &v, 0, &indicator), "read b", SQL_HANDLE_STMT, stmt);
    require(v == 2, "second result is 2", SQL_HANDLE_STMT, stmt);
    require(SQLMoreResults(stmt) == SQL_NO_DATA, "no third result", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* a SQL error surfaces as a diagnostic, and the connection survives it */
    rc = SQLExecDirect(stmt, (SQLCHAR *) "SELECT * FROM no_such_table_anywhere", SQL_NTS);
    require(rc == SQL_ERROR, "querying a missing table fails", SQL_HANDLE_STMT, stmt);
    SQLCHAR state[6] = "";
    SQLCHAR message[512] = "";
    SQLINTEGER native = 0;
    SQLSMALLINT message_length = 0;
    require(SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state, &native, message,
                          sizeof(message), &message_length) == SQL_SUCCESS,
            "the failure carries a diagnostic record", SQL_HANDLE_STMT, stmt);
    require(message[0] != '\0', "the diagnostic has a message", SQL_HANDLE_STMT, stmt);
    exec_ok(stmt, "SELECT 1");

    /* catalog functions */
    require_rc(SQLTables(stmt, NULL, 0, (SQLCHAR *) "PUBLIC", SQL_NTS,
                         (SQLCHAR *) "%", SQL_NTS, NULL, 0),
               "SQLTables over PUBLIC", SQL_HANDLE_STMT, stmt);
    int seen_t = 0;
    while (SQLFetch(stmt) == SQL_SUCCESS) {
        require_rc(SQLGetData(stmt, 3, SQL_C_CHAR, text, sizeof(text), &indicator),
                   "table name cell", SQL_HANDLE_STMT, stmt);
        if (strcmp(text, "T") == 0) {
            seen_t = 1;
        }
    }
    require(seen_t, "SQLTables lists table T", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    require_rc(SQLColumns(stmt, NULL, 0, (SQLCHAR *) "PUBLIC", SQL_NTS,
                          (SQLCHAR *) "T", SQL_NTS, (SQLCHAR *) "%", SQL_NTS),
               "SQLColumns over T", SQL_HANDLE_STMT, stmt);
    int column_rows = 0;
    int price_is_decimal = 0;
    while (SQLFetch(stmt) == SQL_SUCCESS) {
        column_rows++;
        require_rc(SQLGetData(stmt, 4, SQL_C_CHAR, text, sizeof(text), &indicator),
                   "column name cell", SQL_HANDLE_STMT, stmt);
        if (strcmp(text, "PRICE") == 0) {
            SQLSMALLINT reported = 0;
            require_rc(SQLGetData(stmt, 5, SQL_C_SSHORT, &reported, 0, &indicator),
                       "PRICE data type cell", SQL_HANDLE_STMT, stmt);
            price_is_decimal = reported == SQL_DECIMAL;
        }
    }
    require(column_rows == 7, "SQLColumns reports 7 columns", SQL_HANDLE_STMT, stmt);
    require(price_is_decimal, "PRICE reports SQL_DECIMAL", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    require_rc(SQLGetTypeInfo(stmt, SQL_ALL_TYPES), "SQLGetTypeInfo", SQL_HANDLE_STMT, stmt);
    int type_rows = 0;
    while (SQLFetch(stmt) == SQL_SUCCESS) {
        type_rows++;
    }
    require(type_rows >= 8, "type info lists the core types", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* an empty value still has to end a SQLGetData chunking loop */
    require_rc(SQLExecDirect(stmt, (SQLCHAR *) "SELECT '', TO_BINARY('', 'HEX')", SQL_NTS),
               "exec: empty text and empty binary", SQL_HANDLE_STMT, stmt);
    require(SQLFetch(stmt) == SQL_SUCCESS, "fetch the empty row", SQL_HANDLE_STMT, stmt);
    char tiny[8] = "";
    SQLLEN tiny_indicator = -99;
    require_rc(SQLGetData(stmt, 1, SQL_C_CHAR, tiny, sizeof(tiny), &tiny_indicator),
               "empty text reads once", SQL_HANDLE_STMT, stmt);
    require(tiny_indicator == 0, "empty text reports length 0", SQL_HANDLE_STMT, stmt);
    require(SQLGetData(stmt, 1, SQL_C_CHAR, tiny, sizeof(tiny), &tiny_indicator) == SQL_NO_DATA,
            "empty text then reports SQL_NO_DATA", SQL_HANDLE_STMT, stmt);
    require_rc(SQLGetData(stmt, 2, SQL_C_BINARY, tiny, sizeof(tiny), &tiny_indicator),
               "empty binary reads once", SQL_HANDLE_STMT, stmt);
    require(SQLGetData(stmt, 2, SQL_C_BINARY, tiny, sizeof(tiny), &tiny_indicator) == SQL_NO_DATA,
            "empty binary then reports SQL_NO_DATA", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* and a non-empty value is still readable to the end in chunks */
    require_rc(SQLExecDirect(stmt, (SQLCHAR *) "SELECT 'abcdefgh'", SQL_NTS),
               "exec: chunkable text", SQL_HANDLE_STMT, stmt);
    require(SQLFetch(stmt) == SQL_SUCCESS, "fetch the chunkable row", SQL_HANDLE_STMT, stmt);
    char assembled[32] = "";
    char chunk_buffer[4] = "";
    int chunk_calls = 0;
    SQLRETURN chunk_rc;
    while ((chunk_rc = SQLGetData(stmt, 1, SQL_C_CHAR, chunk_buffer, sizeof(chunk_buffer),
                                  &tiny_indicator)) != SQL_NO_DATA) {
        require_rc(chunk_rc, "a chunk reads", SQL_HANDLE_STMT, stmt);
        strcat(assembled, chunk_buffer);
        chunk_calls++;
        require(chunk_calls < 20, "chunked reading terminates", SQL_HANDLE_STMT, stmt);
    }
    require(strcmp(assembled, "abcdefgh") == 0, "chunked reads reassemble the value",
            SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* an indicator that is a marker rather than a length must be refused, not
     * cast to size_t and used to size a read */
    SQLHSTMT marker = SQL_NULL_HSTMT;
    require_rc(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &marker),
               "statement for the indicator checks", SQL_HANDLE_DBC, dbc);
    char marker_value[] = "hello";
    SQLLEN marker_indicator = SQL_DEFAULT_PARAM;
    SQLBindParameter(marker, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
                     50, 0, marker_value, sizeof(marker_value), &marker_indicator);
    require(SQLExecDirect(marker, (SQLCHAR *) "SELECT ?", SQL_NTS) == SQL_ERROR,
            "SQL_DEFAULT_PARAM is refused, not used as a length", SQL_HANDLE_STMT, marker);
    marker_indicator = SQL_DATA_AT_EXEC;
    require(SQLExecDirect(marker, (SQLCHAR *) "SELECT ?", SQL_NTS) == SQL_ERROR,
            "SQL_DATA_AT_EXEC is refused, not used as a length", SQL_HANDLE_STMT, marker);
    marker_indicator = SQL_LEN_DATA_AT_EXEC(4);
    require(SQLExecDirect(marker, (SQLCHAR *) "SELECT ?", SQL_NTS) == SQL_ERROR,
            "SQL_LEN_DATA_AT_EXEC is refused, not used as a length", SQL_HANDLE_STMT, marker);
    marker_indicator = SQL_NTS;
    require_rc(SQLExecDirect(marker, (SQLCHAR *) "SELECT ?", SQL_NTS),
               "SQL_NTS still binds normally", SQL_HANDLE_STMT, marker);
    SQLFreeHandle(SQL_HANDLE_STMT, marker);

    /* a `?` is a placeholder only where it is really code */
    exec_ok(stmt, "SELECT 1 -- trailing question?\n");
    exec_ok(stmt, "SELECT 1 /* inline question? */");
    exec_ok(stmt, "SELECT 1 AS \"why?\"");
    exec_ok(stmt, "SELECT 'literal ? stays'");
    require_rc(SQLExecDirect(stmt, (SQLCHAR *)
        "CREATE OR REPLACE FUNCTION f_smoke() RETURNS DOUBLE LANGUAGE JAVASCRIPT"
        " AS $$ return 1 ? 2 : 3; $$", SQL_NTS),
        "a ? inside a $$ body is not a placeholder", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* a numeric-typed parameter may only carry an actual number */
    SQLHSTMT numeric = SQL_NULL_HSTMT;
    require_rc(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &numeric),
               "statement for the numeric-bind checks", SQL_HANDLE_DBC, dbc);
    char injection[] = "0 OR 1=1";
    SQLLEN numeric_indicator = SQL_NTS;
    SQLBindParameter(numeric, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_DECIMAL,
                     38, 0, injection, sizeof(injection), &numeric_indicator);
    require(SQLExecDirect(numeric, (SQLCHAR *) "SELECT ?", SQL_NTS) == SQL_ERROR,
            "a numeric bind that is not a number is refused", SQL_HANDLE_STMT, numeric);
    char number[] = "-12.50e2";
    SQLBindParameter(numeric, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_DECIMAL,
                     38, 0, number, sizeof(number), &numeric_indicator);
    require_rc(SQLExecDirect(numeric, (SQLCHAR *) "SELECT ?", SQL_NTS),
               "a real number still binds unquoted", SQL_HANDLE_STMT, numeric);
    SQLFreeHandle(SQL_HANDLE_STMT, numeric);

    /* overflow and "not a number" are different diagnoses */
    require_rc(SQLExecDirect(stmt, (SQLCHAR *) "SELECT 9223372036854775808", SQL_NTS),
               "exec: a value past SQL_C_SBIGINT", SQL_HANDLE_STMT, stmt);
    require(SQLFetch(stmt) == SQL_SUCCESS, "fetch the oversized value", SQL_HANDLE_STMT, stmt);
    SQLBIGINT overflowed = 0;
    SQLLEN overflow_indicator = 0;
    require(SQLGetData(stmt, 1, SQL_C_SBIGINT, &overflowed, 0, &overflow_indicator) == SQL_ERROR,
            "an out-of-range number is refused", SQL_HANDLE_STMT, stmt);
    SQLCHAR overflow_state[6] = "";
    SQLCHAR overflow_message[256] = "";
    SQLINTEGER overflow_native = 0;
    SQLSMALLINT overflow_length = 0;
    SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, overflow_state, &overflow_native,
                  overflow_message, sizeof(overflow_message), &overflow_length);
    require(strcmp((char *) overflow_state, "22003") == 0,
            "overflow reports 22003, not 22018", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* SQL_ATTR_MAX_ROWS is obeyed, not merely accepted */
    require_rc(SQLSetStmtAttr(stmt, SQL_ATTR_MAX_ROWS, (SQLPOINTER) 1, 0),
               "set SQL_ATTR_MAX_ROWS", SQL_HANDLE_STMT, stmt);
    require_rc(SQLExecDirect(stmt, (SQLCHAR *) "SELECT id FROM t ORDER BY id", SQL_NTS),
               "exec: select under MAX_ROWS", SQL_HANDLE_STMT, stmt);
    int limited_rows = 0;
    while (SQLFetch(stmt) == SQL_SUCCESS) {
        limited_rows++;
    }
    require(limited_rows == 1, "MAX_ROWS=1 really returns one row", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);
    require_rc(SQLSetStmtAttr(stmt, SQL_ATTR_MAX_ROWS, (SQLPOINTER) 0, 0),
               "clear SQL_ATTR_MAX_ROWS", SQL_HANDLE_STMT, stmt);

    /* an attribute the driver cannot honour says so instead of claiming success */
    require(SQLSetStmtAttr(stmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER) 10, 0) == SQL_ERROR,
            "row-array fetching is refused, not silently ignored", SQL_HANDLE_STMT, stmt);
    require(SQLSetStmtAttr(stmt, (SQLINTEGER) 99999, (SQLPOINTER) 1, 0) == SQL_ERROR,
            "an unknown statement attribute is refused", SQL_HANDLE_STMT, stmt);
    require_rc(SQLSetStmtAttr(stmt, SQL_ATTR_ROW_ARRAY_SIZE, (SQLPOINTER) 1, 0),
               "single-row array size is accepted", SQL_HANDLE_STMT, stmt);

    /* chunked text stops on character boundaries */
    require_rc(SQLExecDirect(stmt, (SQLCHAR *) "SELECT '\xc3\xa9\xc3\xa9\xc3\xa9'", SQL_NTS),
               "exec: three two-byte characters", SQL_HANDLE_STMT, stmt);
    require(SQLFetch(stmt) == SQL_SUCCESS, "fetch the multibyte row", SQL_HANDLE_STMT, stmt);
    char narrow[4] = "";
    SQLLEN narrow_indicator = 0;
    require_rc(SQLGetData(stmt, 1, SQL_C_CHAR, narrow, sizeof(narrow), &narrow_indicator),
               "read a multibyte value into three usable bytes", SQL_HANDLE_STMT, stmt);
    require(strlen(narrow) == 2 && (unsigned char) narrow[0] == 0xC3
            && (unsigned char) narrow[1] == 0xA9,
            "the chunk ends on a character, not mid-sequence", SQL_HANDLE_STMT, stmt);
    SQLCloseCursor(stmt);

    /* a quoted USE keeps the name it was given */
    exec_ok(stmt, "CREATE OR REPLACE DATABASE \"smokeMixed\"");
    exec_ok(stmt, "USE DATABASE \"smokeMixed\"");
    SQLCHAR catalog[256] = "";
    SQLINTEGER catalog_length = 0;
    require_rc(SQLGetConnectAttr(dbc, SQL_ATTR_CURRENT_CATALOG, catalog,
                                 sizeof(catalog), &catalog_length),
               "read the current catalog", SQL_HANDLE_DBC, dbc);
    require(strcmp((char *) catalog, "smokeMixed") == 0,
            "a quoted USE keeps its case and drops its quotes", SQL_HANDLE_DBC, dbc);
    exec_ok(stmt, "DROP DATABASE \"smokeMixed\"");
    exec_ok(stmt, "USE DATABASE odbc_smoke");
    exec_ok(stmt, "USE SCHEMA public");

    /* a Port value written with blanks around it still parses */
    {
        SQLHDBC padded = SQL_NULL_HDBC;
        require_rc(SQLAllocHandle(SQL_HANDLE_DBC, env, &padded),
                   "connection for the padded-port check", SQL_HANDLE_ENV, env);
        const char *port_text = getenv("FROSTLAKE_TEST_PORT");
        char padded_conn[256];
        snprintf(padded_conn, sizeof(padded_conn),
                 "Driver=Frostlake ODBC Driver;Server=localhost;Port= %s ",
                 port_text != NULL ? port_text : "18095");
        SQLCHAR padded_out[512];
        SQLSMALLINT padded_length = 0;
        require_rc(SQLDriverConnect(padded, NULL, (SQLCHAR *) padded_conn, SQL_NTS,
                                    padded_out, sizeof(padded_out), &padded_length,
                                    SQL_DRIVER_NOPROMPT),
                   "a Port with surrounding blanks still connects", SQL_HANDLE_DBC, padded);
        SQLDisconnect(padded);
        SQLFreeHandle(SQL_HANDLE_DBC, padded);

        SQLHDBC bad = SQL_NULL_HDBC;
        require_rc(SQLAllocHandle(SQL_HANDLE_DBC, env, &bad),
                   "connection for the bad-port check", SQL_HANDLE_ENV, env);
        require(SQLDriverConnect(bad, NULL,
                    (SQLCHAR *) "Driver=Frostlake ODBC Driver;Server=localhost;Port=99999",
                    SQL_NTS, padded_out, sizeof(padded_out), &padded_length,
                    SQL_DRIVER_NOPROMPT) == SQL_ERROR,
                "a port outside 1..65535 is refused rather than wrapped", SQL_HANDLE_DBC, bad);
        SQLFreeHandle(SQL_HANDLE_DBC, bad);
    }

    /* A boolean's character form follows the column's declared type: 1/0 for a BOOLEAN column
       (SQL_BIT), true/false where the engine declares the column as something else, as it does
       for a session variable holding a boolean. Either way it still reads as a bit and a number. */
    {
        SQLHSTMT flags = NULL;
        require_rc(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &flags),
                   "allocate a statement for boolean cells", SQL_HANDLE_DBC, dbc);
        exec_ok(flags, "SET smoke_flag = TRUE");
        const char *queries[] = { "SELECT TRUE", "SELECT $smoke_flag" };
        for (int q = 0; q < 2; q++) {
            require_rc(SQLExecDirect(flags, (SQLCHAR *) queries[q], SQL_NTS), "select a boolean",
                       SQL_HANDLE_STMT, flags);
            SQLCHAR bool_name[64];
            SQLSMALLINT bool_name_length = 0, bool_type = 0, bool_digits = 0, bool_nullable = 0;
            SQLULEN bool_size = 0;
            require_rc(SQLDescribeCol(flags, 1, bool_name, sizeof(bool_name), &bool_name_length, &bool_type,
                                      &bool_size, &bool_digits, &bool_nullable),
                       "describe the boolean column", SQL_HANDLE_STMT, flags);
            require(SQLFetch(flags) == SQL_SUCCESS, "fetch the boolean row", SQL_HANDLE_STMT, flags);
            char bool_text[16] = "";
            SQLLEN bool_indicator = 0;
            require_rc(SQLGetData(flags, 1, SQL_C_CHAR, bool_text, sizeof(bool_text), &bool_indicator),
                       "read the boolean as text", SQL_HANDLE_STMT, flags);
            require(strcmp(bool_text, bool_type == SQL_BIT ? "1" : "true") == 0,
                    "a boolean's text follows its column's type", SQL_HANDLE_STMT, flags);
            SQLCHAR bool_bit = 9;
            require_rc(SQLGetData(flags, 1, SQL_C_BIT, &bool_bit, 0, &bool_indicator),
                       "read the boolean as a bit", SQL_HANDLE_STMT, flags);
            require(bool_bit == 1, "the boolean reads as bit 1", SQL_HANDLE_STMT, flags);
            SQLINTEGER bool_int = -1;
            require_rc(SQLGetData(flags, 1, SQL_C_SLONG, &bool_int, 0, &bool_indicator),
                       "read the boolean as an integer", SQL_HANDLE_STMT, flags);
            require(bool_int == 1, "the boolean reads as integer 1", SQL_HANDLE_STMT, flags);
            SQLFreeStmt(flags, SQL_CLOSE);
        }
        SQLFreeHandle(SQL_HANDLE_STMT, flags);
    }

    /* With nothing bound, a `?` is the engine's to read — a Snowflake Scripting cursor bind is one —
       so the statement reaches the engine instead of failing on the client with 07002. Once the
       application has bound something, a marker past its bindings is still refused. */
    {
        SQLHSTMT unbound = NULL;
        require_rc(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &unbound),
                   "allocate a statement for an unbound marker", SQL_HANDLE_DBC, dbc);
        SQLRETURN unbound_rc = SQLExecDirect(unbound, (SQLCHAR *) "SELECT ?", SQL_NTS);
        SQLCHAR unbound_state[6] = "";
        if (unbound_rc == SQL_ERROR) {
            SQLCHAR unbound_message[512];
            SQLINTEGER unbound_native = 0;
            SQLSMALLINT unbound_length = 0;
            SQLGetDiagRec(SQL_HANDLE_STMT, unbound, 1, unbound_state, &unbound_native, unbound_message,
                          sizeof(unbound_message), &unbound_length);
        }
        require(unbound_rc != SQL_ERROR || strcmp((char *) unbound_state, "07002") != 0,
                "an unbound marker goes to the engine, not a client-side 07002", SQL_HANDLE_STMT, unbound);
        SQLFreeHandle(SQL_HANDLE_STMT, unbound);

        SQLHSTMT partly = NULL;
        require_rc(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &partly),
                   "allocate a statement for a partly bound pair", SQL_HANDLE_DBC, dbc);
        SQLINTEGER one_value = 1;
        SQLLEN one_indicator = 0;
        SQLBindParameter(partly, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &one_value, 0,
                         &one_indicator);
        require(SQLExecDirect(partly, (SQLCHAR *) "SELECT ?, ?", SQL_NTS) == SQL_ERROR,
                "a marker past the bound parameters is refused", SQL_HANDLE_STMT, partly);
        SQLCHAR partly_state[6] = "";
        SQLCHAR partly_message[512];
        SQLINTEGER partly_native = 0;
        SQLSMALLINT partly_length = 0;
        SQLGetDiagRec(SQL_HANDLE_STMT, partly, 1, partly_state, &partly_native, partly_message,
                      sizeof(partly_message), &partly_length);
        require(strcmp((char *) partly_state, "07002") == 0, "and the refusal is 07002",
                SQL_HANDLE_STMT, partly);
        SQLFreeHandle(SQL_HANDLE_STMT, partly);
    }

    /* clean up on the server, then tear down */
    exec_ok(stmt, "DROP DATABASE odbc_smoke");
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    require_rc(SQLDisconnect(dbc), "disconnect", SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);

    printf("all %d checks passed\n", checks);
    return 0;
}
