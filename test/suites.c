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
 * The engine's testkit corpus — language-neutral JSON suites (SCHEMA.md beside them) — run through this
 * driver, the way an ODBC application reaches the engine: every statement goes through the unixODBC
 * driver manager into build/libfrostlakeodbc.so. A port of the Java reference (JsonSuiteTest, Compare,
 * HttpBackend, ExecResult, SemiStructuredCells); status for status it should agree with the other
 * drivers' runners and their TSV.
 *
 *   make build/suites    (or: cc -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -Isrc test/suites.c src/json.c
 *                         -o build/suites -lodbc)
 *   FL_CORPUS=/path/to/frostlake/engine/src/test/resources/testkit \
 *   FROSTLAKE_CLASSPATH=<engine runtime classpath> build/suites
 *
 * `make test` runs it too, against the smoke test's server, whenever FL_CORPUS is set.
 *
 * Environment:
 *   FL_CORPUS                the testkit directory whose suites directory holds the *.json to run;
 *                            unset or empty, the runner says so and exits 0 having run nothing
 *   FROSTLAKE_URL            attach to a running server (http://host:port), or
 *   FROSTLAKE_CLASSPATH      boot a private one: java -cp <it> with its own -Duser.home and working
 *                            directory, logging to a file (never a pipe — an undrained pipe stalls it)
 *   TESTKIT_SUITES           comma-separated words; only suites whose name contains one run
 *   FROSTLAKE_TESTKIT_REPORT the TSV to write (default $TMPDIR/testkit-odbc.tsv)
 *   FROSTLAKE_ODBC_DRIVER    the driver library (default build/libfrostlakeodbc.so)
 *
 * Backend names for a case's skip list: "odbc", and "http" — this driver rides the HTTP wire, so the
 * wire's own divergences reach it too. Capabilities: COLUMN_NAMES, UPDATE_COUNT, SESSION; no
 * ERROR_CODE, because the driver reports every engine refusal under one SQLSTATE.
 *
 * Where ODBC differs from the wire, the runner reads the value the way an ODBC application must:
 *   - A BOOLEAN column is SQL_BIT, whose character form is "1"/"0" by the ODBC specification; the
 *     suites record true/false, so a SQL_BIT cell reads as the boolean it carries.
 *   - A DML count is SQLRowCount's, taken when the result is the count grid that the driver itself
 *     reads a count from (a single row, every column "number of …", one of them "number of rows …").
 *     Any other shape falls back to the reference's grid derivation.
 */

#include <sql.h>
#include <sqlext.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json.h"

static const char *const RESET_CONTEXT[] = {
    "ALTER SESSION SET MULTI_STATEMENT_COUNT = 0",
    "CREATE OR REPLACE DATABASE test_db",
    "USE DATABASE test_db",
    "CREATE OR REPLACE SCHEMA test_schema",
    "USE SCHEMA test_schema",
};

/* ---- small helpers ------------------------------------------------------------------------------- */

static void *xmalloc(size_t size) {
    void *p = malloc(size > 0 ? size : 1);
    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(3);
    }
    return p;
}

static char *xstrndup(const char *s, size_t n) {
    char *copy = xmalloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

static char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

static char *fmt(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *out = NULL;
    if (vasprintf(&out, format, args) < 0) {
        fprintf(stderr, "out of memory\n");
        exit(3);
    }
    va_end(args);
    return out;
}

typedef struct strvec {
    char **items;
    size_t count;
    size_t capacity;
} strvec;

static void strvec_push(strvec *vec, char *owned) {
    if (vec->count == vec->capacity) {
        vec->capacity = vec->capacity == 0 ? 8 : vec->capacity * 2;
        char **grown = realloc(vec->items, vec->capacity * sizeof(char *));
        if (grown == NULL) {
            fprintf(stderr, "out of memory\n");
            exit(3);
        }
        vec->items = grown;
    }
    vec->items[vec->count++] = owned;
}

static void strvec_free(strvec *vec) {
    for (size_t i = 0; i < vec->count; i++) {
        free(vec->items[i]);
    }
    free(vec->items);
    vec->items = NULL;
    vec->count = vec->capacity = 0;
}

static int starts_with_ignore_case(const char *text, const char *prefix) {
    return text != NULL && strncasecmp(text, prefix, strlen(prefix)) == 0;
}

/* Case folding for the reference's equalsIgnoreCase / toLowerCase(Locale.ROOT): ASCII plus the
 * alphabets identifiers and messages actually use (Latin-1, Latin Extended-A, Greek, Cyrillic). */
static unsigned long fold_codepoint(unsigned long cp) {
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 32;
    }
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) {
        return cp + 32;
    }
    if (cp >= 0x100 && cp <= 0x17F && cp != 0x130 && cp != 0x131 && cp != 0x138 && cp != 0x149
        && cp != 0x17F) {
        if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
            return cp % 2 == 1 ? cp + 1 : cp;
        }
        return cp % 2 == 0 ? cp + 1 : cp;
    }
    if (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2) {
        return cp + 32;
    }
    if (cp >= 0x410 && cp <= 0x42F) {
        return cp + 32;
    }
    if (cp >= 0x400 && cp <= 0x40F) {
        return cp + 80;
    }
    return cp;
}

static char *fold_case(const char *text) {
    size_t length = strlen(text);
    char *out = xmalloc(length * 2 + 1);
    size_t o = 0;
    const unsigned char *s = (const unsigned char *) text;
    size_t i = 0;
    while (i < length) {
        unsigned long cp;
        size_t width;
        if (s[i] < 0x80) {
            cp = s[i];
            width = 1;
        } else if ((s[i] & 0xE0) == 0xC0 && i + 1 < length && (s[i + 1] & 0xC0) == 0x80) {
            cp = ((unsigned long) (s[i] & 0x1F) << 6) | (s[i + 1] & 0x3F);
            width = 2;
        } else {
            out[o++] = (char) s[i++];
            continue;
        }
        unsigned long folded = fold_codepoint(cp);
        if (folded < 0x80) {
            out[o++] = (char) folded;
        } else {
            out[o++] = (char) (0xC0 | (folded >> 6));
            out[o++] = (char) (0x80 | (folded & 0x3F));
        }
        i += width;
    }
    out[o] = '\0';
    return out;
}

static int equals_ignore_case(const char *a, const char *b) {
    char *fa = fold_case(a);
    char *fb = fold_case(b);
    int same = strcmp(fa, fb) == 0;
    free(fa);
    free(fb);
    return same;
}

static int contains_ignore_case(const char *haystack, const char *needle) {
    char *fh = fold_case(haystack);
    char *fn = fold_case(needle);
    int found = strstr(fh, fn) != NULL;
    free(fh);
    free(fn);
    return found;
}

/* ---- Compare.norm ----------------------------------------------------------------------------------- */

#define NORM_DIGITS 10
/* An exponent past this is text, not a number: the reference's BigDecimal overflows first. */
#define MAX_EXPONENT 100000L

/* The BigDecimal reading of `s`: rounded HALF_UP to ten significant digits, trailing zeros stripped,
 * in plain notation. NULL when `s` is not a BigDecimal literal. */
static char *plain_number(const char *s, size_t length) {
    size_t i = 0;
    int negative = 0;
    if (i < length && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        i++;
    }
    char *digits = xmalloc(length + 1);
    size_t count = 0;
    long fraction = 0;
    int seen = 0;
    while (i < length && s[i] >= '0' && s[i] <= '9') {
        digits[count++] = s[i++];
        seen = 1;
    }
    if (i < length && s[i] == '.') {
        i++;
        while (i < length && s[i] >= '0' && s[i] <= '9') {
            digits[count++] = s[i++];
            fraction++;
            seen = 1;
        }
    }
    if (!seen) {
        free(digits);
        return NULL;
    }
    long exponent = 0;
    if (i < length && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        int exponent_negative = 0;
        if (i < length && (s[i] == '+' || s[i] == '-')) {
            exponent_negative = s[i] == '-';
            i++;
        }
        int exponent_seen = 0;
        while (i < length && s[i] >= '0' && s[i] <= '9') {
            if (exponent <= MAX_EXPONENT) {
                exponent = exponent * 10 + (s[i] - '0');
            }
            i++;
            exponent_seen = 1;
        }
        if (!exponent_seen || exponent > MAX_EXPONENT) {
            free(digits);
            return NULL;
        }
        if (exponent_negative) {
            exponent = -exponent;
        }
    }
    if (i != length) {
        free(digits);
        return NULL;
    }

    /* value = d * 10^-scale */
    long scale = fraction - exponent;
    size_t start = 0;
    while (start < count && digits[start] == '0') {
        start++;
    }
    if (start == count) {
        free(digits);
        return xstrdup("0");
    }
    char *d = digits + start;
    size_t n = count - start;
    if (n > NORM_DIGITS) {
        int round_up = d[NORM_DIGITS] >= '5';
        scale -= (long) (n - NORM_DIGITS);
        n = NORM_DIGITS;
        if (round_up) {
            size_t k = n;
            while (k > 0 && d[k - 1] == '9') {
                d[k - 1] = '0';
                k--;
            }
            if (k > 0) {
                d[k - 1]++;
            } else {
                /* 9999999999 rounded up needs an eleventh digit: 1000000000 one scale lower. */
                d[0] = '1';
                scale -= 1;
            }
        }
    }
    while (n > 1 && d[n - 1] == '0') {
        n--;
        scale--;
    }

    size_t magnitude = (size_t) (scale < 0 ? -scale : scale);
    char *out = xmalloc(n + magnitude + 4);
    size_t o = 0;
    if (negative) {
        out[o++] = '-';
    }
    if (scale <= 0) {
        memcpy(out + o, d, n);
        o += n;
        for (long z = 0; z < -scale; z++) {
            out[o++] = '0';
        }
    } else if ((long) n > scale) {
        size_t point = n - (size_t) scale;
        memcpy(out + o, d, point);
        o += point;
        out[o++] = '.';
        memcpy(out + o, d + point, (size_t) scale);
        o += (size_t) scale;
    } else {
        out[o++] = '0';
        out[o++] = '.';
        for (long z = 0; z < scale - (long) n; z++) {
            out[o++] = '0';
        }
        memcpy(out + o, d, n);
        o += n;
    }
    out[o] = '\0';
    free(digits);
    return out;
}

/* Compare.norm: NULL/empty -> NULL, booleans upper-cased, numbers via BigDecimal, else trimmed text. */
static char *norm(const char *raw) {
    if (raw == NULL) {
        return xstrdup("NULL");
    }
    const char *b = raw;
    const char *e = raw + strlen(raw);
    while (b < e && (unsigned char) *b <= 0x20) {
        b++;
    }
    while (e > b && (unsigned char) e[-1] <= 0x20) {
        e--;
    }
    size_t length = (size_t) (e - b);
    if (length == 0 || (length == 4 && strncasecmp(b, "null", 4) == 0)) {
        return xstrdup("NULL");
    }
    if (length == 4 && strncasecmp(b, "true", 4) == 0) {
        return xstrdup("TRUE");
    }
    if (length == 5 && strncasecmp(b, "false", 5) == 0) {
        return xstrdup("FALSE");
    }
    char *number = plain_number(b, length);
    return number != NULL ? number : xstrndup(b, length);
}

/* Compare.asString for an expectation node: a number's lexeme stands in for BigDecimal.toPlainString,
 * which norm() reads identically. */
static const char *as_string(const fl_json *node) {
    if (node == NULL || node->kind == FL_JSON_NULL) {
        return "null";
    }
    if (node->kind == FL_JSON_STRING || node->kind == FL_JSON_NUMBER || node->kind == FL_JSON_BOOL) {
        return node->text;
    }
    return node->kind == FL_JSON_ARRAY ? "[array]" : "{object}";
}

/* ---- one statement through ODBC: ExecResult ------------------------------------------------------- */

typedef struct exec_result {
    int has_columns;
    strvec columns;
    int has_rows;
    size_t width;
    size_t row_count;
    size_t row_capacity;
    char ***rows;          /* rows[r][c]; a NULL cell is SQL NULL */
    long long update_count; /* -1 when none */
    char *error;           /* the refusal, when the statement was refused */
    char *transport;       /* a transport failure: the test is an ERROR, never an expected refusal */
} exec_result;

static void exec_result_free(exec_result *r) {
    strvec_free(&r->columns);
    for (size_t i = 0; i < r->row_count; i++) {
        for (size_t c = 0; c < r->width; c++) {
            free(r->rows[i][c]);
        }
        free(r->rows[i]);
    }
    free(r->rows);
    free(r->error);
    free(r->transport);
    memset(r, 0, sizeof(*r));
}

static char *diagnostic(SQLSMALLINT type, SQLHANDLE handle, char state[6]) {
    SQLCHAR sqlstate[6] = "";
    SQLCHAR message[16384] = "";
    SQLINTEGER native = 0;
    SQLSMALLINT length = 0;
    SQLRETURN rc = SQLGetDiagRec(type, handle, 1, sqlstate, &native, message, sizeof(message), &length);
    if (state != NULL) {
        memcpy(state, SQL_SUCCEEDED(rc) ? (char *) sqlstate : "HY000", 6);
        state[5] = '\0';
    }
    return SQL_SUCCEEDED(rc) ? xstrdup((char *) message) : xstrdup("(no diagnostic record)");
}

static char *read_cell(SQLHSTMT stmt, SQLUSMALLINT column, int *is_null, char **failure) {
    fl_strbuf text;
    fl_strbuf_init(&text);
    static char chunk[65536];
    *is_null = 0;
    for (;;) {
        SQLLEN indicator = 0;
        SQLRETURN rc = SQLGetData(stmt, column, SQL_C_CHAR, chunk, sizeof(chunk), &indicator);
        if (rc == SQL_NO_DATA) {
            break;
        }
        if (!SQL_SUCCEEDED(rc)) {
            char *why = diagnostic(SQL_HANDLE_STMT, stmt, NULL);
            *failure = fmt("SQLGetData on column %u: %s", (unsigned) column, why);
            free(why);
            fl_strbuf_free(&text);
            return NULL;
        }
        if (indicator == SQL_NULL_DATA) {
            *is_null = 1;
            fl_strbuf_free(&text);
            return NULL;
        }
        fl_strbuf_append(&text, chunk);
        if (rc == SQL_SUCCESS) {
            break;
        }
    }
    return text.data != NULL ? text.data : xstrdup("");
}

/* SemiStructuredCells.value: a JSON string's content, anything else unchanged. */
static char *semi_structured_value(char *cell) {
    fl_json *parsed = fl_json_parse(cell, strlen(cell));
    if (parsed == NULL) {
        return cell;
    }
    if (parsed->kind == FL_JSON_STRING) {
        char *content = xstrdup(parsed->text != NULL ? parsed->text : "");
        fl_json_free(parsed);
        free(cell);
        return content;
    }
    fl_json_free(parsed);
    return cell;
}

static void read_grid(SQLHSTMT stmt, SQLSMALLINT width, exec_result *out) {
    out->has_columns = 1;
    out->has_rows = 1;
    out->width = (size_t) width;
    int *is_bit = calloc((size_t) width, sizeof(int));
    int *is_semi = calloc((size_t) width, sizeof(int));
    for (SQLSMALLINT i = 1; i <= width; i++) {
        SQLCHAR name[4096] = "";
        SQLSMALLINT name_length = 0, type = 0, digits = 0, nullable = 0;
        SQLULEN size = 0;
        SQLDescribeCol(stmt, (SQLUSMALLINT) i, name, sizeof(name), &name_length, &type, &size, &digits,
                       &nullable);
        strvec_push(&out->columns, xstrdup((char *) name));
        SQLCHAR type_name[256] = "";
        SQLSMALLINT type_name_length = 0;
        SQLColAttribute(stmt, (SQLUSMALLINT) i, SQL_DESC_TYPE_NAME, type_name, sizeof(type_name),
                        &type_name_length, NULL);
        is_bit[i - 1] = type == SQL_BIT;
        is_semi[i - 1] = strcasecmp((char *) type_name, "VARIANT") == 0
            || strcasecmp((char *) type_name, "OBJECT") == 0
            || strcasecmp((char *) type_name, "ARRAY") == 0;
    }
    for (;;) {
        SQLRETURN rc = SQLFetch(stmt);
        if (rc == SQL_NO_DATA) {
            break;
        }
        if (!SQL_SUCCEEDED(rc)) {
            char *why = diagnostic(SQL_HANDLE_STMT, stmt, NULL);
            out->transport = fmt("SQLFetch: %s", why);
            free(why);
            break;
        }
        char **row = calloc((size_t) width, sizeof(char *));
        for (SQLSMALLINT c = 0; c < width; c++) {
            int is_null = 0;
            char *failure = NULL;
            char *cell = read_cell(stmt, (SQLUSMALLINT) (c + 1), &is_null, &failure);
            if (failure != NULL) {
                out->transport = failure;
                for (SQLSMALLINT k = 0; k < c; k++) {
                    free(row[k]);
                }
                free(row);
                free(is_bit);
                free(is_semi);
                return;
            }
            if (!is_null && is_bit[c]) {
                if (strcmp(cell, "1") == 0) {
                    free(cell);
                    cell = xstrdup("true");
                } else if (strcmp(cell, "0") == 0) {
                    free(cell);
                    cell = xstrdup("false");
                }
            }
            if (!is_null && is_semi[c]) {
                cell = semi_structured_value(cell);
            }
            row[c] = cell;
        }
        if (out->row_count == out->row_capacity) {
            out->row_capacity = out->row_capacity == 0 ? 16 : out->row_capacity * 2;
            out->rows = realloc(out->rows, out->row_capacity * sizeof(char **));
        }
        out->rows[out->row_count++] = row;
    }
    free(is_bit);
    free(is_semi);
}

/* The count grid the driver reads SQLRowCount from: one row, every column "number of …", at least one
 * "number of rows …". Its count is what an ODBC application is given for the DML. */
static void count_from_row_count(SQLHSTMT stmt, exec_result *out) {
    if (!out->has_columns || out->columns.count == 0 || out->row_count != 1) {
        return;
    }
    int counts_rows = 0;
    for (size_t i = 0; i < out->columns.count; i++) {
        if (!starts_with_ignore_case(out->columns.items[i], "number of")) {
            return;
        }
        counts_rows = counts_rows || starts_with_ignore_case(out->columns.items[i], "number of rows");
    }
    SQLLEN count = -1;
    if (counts_rows && SQL_SUCCEEDED(SQLRowCount(stmt, &count)) && count >= 0) {
        out->update_count = (long long) count;
    }
}

/* ExecResult.deriveUpdateCountFromGrid */
static void derive_update_count(exec_result *out) {
    if (out->update_count >= 0 || !out->has_columns || out->columns.count == 0 || !out->has_rows
        || out->row_count != 1) {
        return;
    }
    for (size_t i = 0; i < out->columns.count; i++) {
        if (!starts_with_ignore_case(out->columns.items[i], "number of")) {
            return;
        }
    }
    const char *cell = out->rows[0][0];
    if (cell == NULL) {
        return;
    }
    while (*cell != '\0' && (unsigned char) *cell <= 0x20) {
        cell++;
    }
    char *end = NULL;
    errno = 0;
    long long value = strtoll(cell, &end, 10);
    while (end != NULL && *end != '\0' && (unsigned char) *end <= 0x20) {
        end++;
    }
    if (end != cell && end != NULL && *end == '\0' && errno == 0) {
        out->update_count = value;
    }
}

static void execute(SQLHDBC dbc, const char *sql, exec_result *out) {
    memset(out, 0, sizeof(*out));
    out->update_count = -1;
    SQLHSTMT stmt = SQL_NULL_HSTMT;
    if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt))) {
        out->transport = diagnostic(SQL_HANDLE_DBC, dbc, NULL);
        return;
    }
    SQLRETURN rc = SQLExecDirect(stmt, (SQLCHAR *) sql, SQL_NTS);
    if (rc == SQL_ERROR) {
        char state[6];
        char *message = diagnostic(SQL_HANDLE_STMT, stmt, state);
        if (strncmp(state, "08", 2) == 0) {
            out->transport = message;
        } else {
            out->error = message;
        }
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return;
    }
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO && rc != SQL_NO_DATA) {
        out->transport = fmt("SQLExecDirect returned %d", (int) rc);
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return;
    }
    SQLSMALLINT width = 0;
    SQLNumResultCols(stmt, &width);
    if (width > 0) {
        read_grid(stmt, width, out);
    }
    if (out->transport == NULL) {
        count_from_row_count(stmt, out);
        derive_update_count(out);
    }
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
}

/* ---- Compare.check ------------------------------------------------------------------------------ */

static int compare_lines(const void *a, const void *b) {
    return strcmp(*(char *const *) a, *(char *const *) b);
}

static char *canon_line(const fl_strbuf *line) {
    return line->data != NULL ? xstrdup(line->data) : xstrdup("");
}

static char *join_lines(char **lines, size_t count) {
    fl_strbuf out;
    fl_strbuf_init(&out);
    fl_strbuf_append(&out, "[");
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            fl_strbuf_append(&out, ", ");
        }
        for (const char *c = lines[i]; *c != '\0'; c++) {
            fl_strbuf_append_len(&out, *c == 0x1f ? "|" : c, 1);
        }
    }
    fl_strbuf_append(&out, "]");
    return out.data;
}

static char *grid_diff(const fl_json *want, const exec_result *got, int ordered) {
    size_t expected_count = want->child_count;
    char **expected = xmalloc(expected_count * sizeof(char *));
    for (size_t r = 0; r < expected_count; r++) {
        const fl_json *row = want->children[r];
        fl_strbuf line;
        fl_strbuf_init(&line);
        for (size_t c = 0; row != NULL && c < row->child_count; c++) {
            const fl_json *cell = row->children[c];
            char *normalized = norm(cell == NULL || cell->kind == FL_JSON_NULL ? NULL : as_string(cell));
            fl_strbuf_append(&line, normalized);
            fl_strbuf_append(&line, "\x1f");
            free(normalized);
        }
        expected[r] = canon_line(&line);
        fl_strbuf_free(&line);
    }
    size_t actual_count = got->has_rows ? got->row_count : 0;
    char **actual = xmalloc(actual_count * sizeof(char *));
    for (size_t r = 0; r < actual_count; r++) {
        fl_strbuf line;
        fl_strbuf_init(&line);
        for (size_t c = 0; c < got->width; c++) {
            char *normalized = norm(got->rows[r][c]);
            fl_strbuf_append(&line, normalized);
            fl_strbuf_append(&line, "\x1f");
            free(normalized);
        }
        actual[r] = canon_line(&line);
        fl_strbuf_free(&line);
    }
    if (!ordered) {
        qsort(expected, expected_count, sizeof(char *), compare_lines);
        qsort(actual, actual_count, sizeof(char *), compare_lines);
    }
    int same = expected_count == actual_count;
    for (size_t i = 0; same && i < expected_count; i++) {
        same = strcmp(expected[i], actual[i]) == 0;
    }
    char *detail = NULL;
    if (!same) {
        char *e = join_lines(expected, expected_count);
        char *a = join_lines(actual, actual_count);
        detail = fmt("rows differ: expected %s got %s", e, a);
        free(e);
        free(a);
    }
    for (size_t i = 0; i < expected_count; i++) {
        free(expected[i]);
    }
    for (size_t i = 0; i < actual_count; i++) {
        free(actual[i]);
    }
    free(expected);
    free(actual);
    return detail;
}

static char *check_refusal(const fl_json *error, const exec_result *r, int *capability_skips) {
    if (r->error == NULL) {
        return xstrdup("expected an error, statement succeeded");
    }
    const char *want = fl_json_get_string(error, "messageContains");
    if (want != NULL && !contains_ignore_case(r->error, want)) {
        return fmt("error message [%s] does not contain [%s]", r->error, want);
    }
    if (fl_json_get_string(error, "code") != NULL || fl_json_get_string(error, "sqlState") != NULL) {
        (*capability_skips)++; /* ERROR_CODE: the driver reports every refusal under one SQLSTATE */
    }
    return NULL;
}

/* Compare.check: NULL when the step passes, else why it failed. */
static char *check(const fl_json *expect, const exec_result *r, int *capability_skips) {
    const fl_json *error = fl_json_get(expect, "error");
    if (error != NULL && error->kind == FL_JSON_OBJECT) {
        return check_refusal(error, r, capability_skips);
    }
    if (r->error != NULL) {
        return fmt("unexpected error: %s", r->error);
    }
    if (expect == NULL || expect->kind != FL_JSON_OBJECT) {
        return NULL;
    }
    const fl_json *value = fl_json_get(expect, "value");
    if (value != NULL) {
        const char *want = as_string(value);
        const char *got = r->has_rows && r->row_count > 0 && r->width > 0 ? r->rows[0][0] : NULL;
        char *nw = norm(want);
        char *ng = norm(got);
        int same = strcmp(nw, ng) == 0;
        free(nw);
        free(ng);
        if (!same) {
            return fmt("value [%s] != expected [%s]", got != NULL ? got : "null", want);
        }
    }
    const fl_json *rows = fl_json_get(expect, "rows");
    if (rows != NULL && rows->kind == FL_JSON_ARRAY) {
        const fl_json *ordered = fl_json_get(expect, "ordered");
        int is_ordered = ordered != NULL && ordered->kind == FL_JSON_BOOL && strcmp(ordered->text, "true") == 0;
        char *diff = grid_diff(rows, r, is_ordered);
        if (diff != NULL) {
            return diff;
        }
    }
    const fl_json *row_count = fl_json_get(expect, "rowCount");
    if (row_count != NULL && row_count->kind == FL_JSON_NUMBER) {
        long long want = atoll(row_count->text);
        long long got = r->has_rows ? (long long) r->row_count : 0;
        if (got != want) {
            return fmt("rowCount %lld != expected %lld", got, want);
        }
    }
    const fl_json *columns = fl_json_get(expect, "columns");
    if (columns != NULL && columns->kind == FL_JSON_ARRAY) {
        size_t got = r->has_columns ? r->columns.count : 0;
        if (got != columns->child_count) {
            return fmt("column count %zu != expected %zu", got, columns->child_count);
        }
        for (size_t i = 0; i < got; i++) {
            const char *want = as_string(columns->children[i]);
            if (!equals_ignore_case(want, r->columns.items[i])) {
                return fmt("column[%zu] [%s] != expected [%s]", i, r->columns.items[i], want);
            }
        }
    }
    const fl_json *update_count = fl_json_get(expect, "updateCount");
    if (update_count != NULL && update_count->kind == FL_JSON_NUMBER) {
        long long want = atoll(update_count->text);
        if (r->update_count != want) {
            return fmt("updateCount %lld != expected %lld", r->update_count, want);
        }
    }
    return NULL;
}

/* ---- norm self-check: the reference's own answers, generated from Java's BigDecimal ---------------- */

static const char *const NORM_CASES[][2] = {
    { "2", "2" },
    { "2.000000", "2" },
    { "3.500000", "3.5" },
    { " 42 ", "42" },
    { "-0", "0" },
    { "0.000", "0" },
    { "1e+21", "1000000000000000000000" },
    { "2.5e-05", "0.000025" },
    { "0.30000000000000004", "0.3" },
    { "12345678905", "12345678910" },
    { "9999999999.5", "10000000000" },
    { "-12345678901234567890123456789", "-12345678900000000000000000000" },
    { "1.", "1" },
    { ".5", "0.5" },
    { "+7", "7" },
    { "1e", "1e" },
    { "0x10", "0x10" },
    { "NaN", "NaN" },
    { "true", "TRUE" },
    { "False", "FALSE" },
    { "null", "NULL" },
    { "", "NULL" },
    { "abc", "abc" },
    { "1.0E10", "10000000000" },
    { "1.0E-5", "0.00001" },
    { "-1.5E-7", "-0.00000015" },
    { "123456789012345678901234567890.123456789", "123456789000000000000000000000" },
    { "0.00000000012345678915", "0.0000000001234567892" },
    { "0.99999999995", "1" },
    { "-9999999999.5", "-10000000000" },
    { "1E+3", "1000" },
    { "1e0", "1" },
    { "+.5", "0.5" },
    { ".", "." },
    { "-", "-" },
    { "+", "+" },
    { "1e+", "1e+" },
    { "1e-", "1e-" },
    { "1.2.3", "1.2.3" },
    { "1e5.5", "1e5.5" },
    { "--1", "--1" },
    { "Infinity", "Infinity" },
    { "-Infinity", "-Infinity" },
    { "inf", "inf" },
    { "nan", "nan" },
    { "5e-30", "0.000000000000000000000000000005" },
    { "1.7976931348623157E30", "1797693135000000000000000000000" },
    { "\t7\n", "7" },
    { "TRUE ", "TRUE" },
    { "NULL", "NULL" },
    { "nuLL", "NULL" },
    { "00012", "12" },
    { "-0.0", "0" },
    { "0e10", "0" },
    { "12345678901", "12345678900" },
    { "1234567890.5", "1234567891" },
    { "1234567890.4999", "1234567890" },
    { "100", "100" },
    { "1.000000000000000000001", "1" },
    { "6.02214076e23", "602214076000000000000000" },
    { "DEADBEEF", "DEADBEEF" },
    { "0123", "123" },
    { "-00.00100", "-0.001" },
    { "1 2", "1 2" },
    { "12e-3", "0.012" },
    { "1234.5678e2", "123456.78" },
};

static int check_norm(void) {
    int failures = 0;
    for (size_t i = 0; i < sizeof(NORM_CASES) / sizeof(NORM_CASES[0]); i++) {
        char *got = norm(NORM_CASES[i][0]);
        if (strcmp(got, NORM_CASES[i][1]) != 0) {
            fprintf(stderr, "norm(\"%s\") = \"%s\", reference says \"%s\"\n", NORM_CASES[i][0], got,
                    NORM_CASES[i][1]);
            failures++;
        }
        free(got);
    }
    return failures;
}

/* ---- server, connection, suites ----------------------------------------------------------------- */

static pid_t server_pid = -1;

static void stop_server(void) {
    if (server_pid > 0) {
        kill(server_pid, SIGTERM);
        waitpid(server_pid, NULL, 0);
        server_pid = -1;
    }
}

static void on_signal(int signal_number) {
    stop_server();
    _exit(128 + signal_number);
}

static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof(address);
    int port = -1;
    if (fd >= 0 && bind(fd, (struct sockaddr *) &address, sizeof(address)) == 0
        && getsockname(fd, (struct sockaddr *) &address, &length) == 0) {
        port = ntohs(address.sin_port);
    }
    if (fd >= 0) {
        close(fd);
    }
    return port;
}

static void boot_server(const char *classpath, const char *workdir, int port) {
    char *home = fmt("%s/home", workdir);
    char *log = fmt("%s/server.log", workdir);
    mkdir(home, 0700);
    const char *java_home = getenv("JAVA_HOME");
    char *java = java_home != NULL && java_home[0] != '\0' ? fmt("%s/bin/java", java_home) : xstrdup("java");
    char *user_home = fmt("-Duser.home=%s", home);
    char *port_text = fmt("%d", port);
    server_pid = fork();
    if (server_pid == 0) {
        if (chdir(workdir) != 0) {
            _exit(127);
        }
        int out = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int in = open("/dev/null", O_RDONLY);
        if (out < 0 || in < 0) {
            _exit(127);
        }
        dup2(in, 0);
        dup2(out, 1);
        dup2(out, 2);
        execlp(java, java, user_home, "-cp", classpath, "dev.frostlake.http.DatabaseHttpServer", port_text,
               (char *) NULL);
        _exit(127);
    }
    free(home);
    free(log);
    free(java);
    free(user_home);
    free(port_text);
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *) a, *(char *const *) b);
}

static char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = xmalloc((size_t) size + 1);
    *length = fread(data, 1, (size_t) size, file);
    data[*length] = '\0';
    fclose(file);
    return data;
}

static int wanted(const char *filter, const char *suite) {
    if (filter == NULL || filter[0] == '\0') {
        return 1;
    }
    char *words = xstrdup(filter);
    int found = 0;
    for (char *save = NULL, *word = strtok_r(words, ",", &save); word != NULL && !found;
         word = strtok_r(NULL, ",", &save)) {
        while (*word == ' ') {
            word++;
        }
        size_t n = strlen(word);
        while (n > 0 && word[n - 1] == ' ') {
            word[--n] = '\0';
        }
        found = n > 0 && contains_ignore_case(suite, word);
    }
    free(words);
    return found;
}

static const char *skip_reason(const fl_json *test) {
    const fl_json *skip = fl_json_get(test, "skip");
    const fl_json *backends = fl_json_get(skip, "backends");
    if (backends == NULL || backends->kind != FL_JSON_ARRAY) {
        return NULL;
    }
    for (size_t i = 0; i < backends->child_count; i++) {
        const fl_json *name = backends->children[i];
        if (name != NULL && name->kind == FL_JSON_STRING
            && (strcasecmp(name->text, "odbc") == 0 || strcasecmp(name->text, "http") == 0)) {
            const char *reason = fl_json_get_string(skip, "reason");
            return reason != NULL ? reason : "null";
        }
    }
    return NULL;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static char *one_line(const char *text) {
    char *copy = xstrdup(text);
    for (char *c = copy; *c != '\0'; c++) {
        if (*c == '\t' || *c == '\n' || *c == '\r') {
            *c = ' ';
        }
    }
    return copy;
}

typedef struct verdict {
    const char *status;
    long step; /* 0 = none */
    char *detail;
} verdict;

static verdict run_test(SQLHDBC dbc, const fl_json *test, int *capability_skips) {
    verdict v = { "PASS", 0, NULL };
    for (size_t i = 0; i < sizeof(RESET_CONTEXT) / sizeof(RESET_CONTEXT[0]); i++) {
        exec_result r;
        execute(dbc, RESET_CONTEXT[i], &r);
        if (r.transport != NULL || r.error != NULL) {
            v.status = "ERROR";
            v.detail = fmt("resetContext failed on '%s': %s", RESET_CONTEXT[i],
                           r.transport != NULL ? r.transport : r.error);
            exec_result_free(&r);
            return v;
        }
        exec_result_free(&r);
    }
    const fl_json *steps = fl_json_get(test, "steps");
    for (size_t i = 0; steps != NULL && steps->kind == FL_JSON_ARRAY && i < steps->child_count; i++) {
        const fl_json *step = steps->children[i];
        const char *sql = fl_json_get_string(step, "sql");
        if (sql == NULL) {
            sql = "";
        }
        exec_result r;
        execute(dbc, sql, &r);
        if (r.transport != NULL) {
            v.status = "ERROR";
            v.step = (long) i + 1;
            v.detail = fmt("%s  [sql: %s]", r.transport, sql);
            exec_result_free(&r);
            return v;
        }
        char *failure = check(fl_json_get(step, "expect"), &r, capability_skips);
        exec_result_free(&r);
        if (failure != NULL) {
            v.status = "FAIL";
            v.step = (long) i + 1;
            v.detail = fmt("%s  [sql: %s]", failure, sql);
            free(failure);
            return v;
        }
    }
    return v;
}

static SQLHDBC connect_driver(SQLHENV env, const char *connection, int attempts) {
    for (int attempt = 0; attempt < attempts; attempt++) {
        SQLHDBC dbc = SQL_NULL_HDBC;
        SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
        SQLCHAR out[1024];
        SQLSMALLINT out_length = 0;
        if (SQL_SUCCEEDED(SQLDriverConnect(dbc, NULL, (SQLCHAR *) connection, SQL_NTS, out, sizeof(out),
                                           &out_length, SQL_DRIVER_NOPROMPT))) {
            return dbc;
        }
        if (attempt + 1 == attempts) {
            char *why = diagnostic(SQL_HANDLE_DBC, dbc, NULL);
            fprintf(stderr, "cannot connect: %s\n", why);
            free(why);
        }
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        if (server_pid > 0 && waitpid(server_pid, NULL, WNOHANG) == server_pid) {
            server_pid = -1;
            fprintf(stderr, "the server exited during startup — see its server.log\n");
            return SQL_NULL_HDBC;
        }
        usleep(250000);
    }
    return SQL_NULL_HDBC;
}

int main(void) {
    /* The corpus comes before the engine: without FL_CORPUS there is nothing to replay, and a
     * directory holding no suites is a caller pointing at the wrong place. */
    const char *corpus = getenv("FL_CORPUS");
    if (corpus == NULL || corpus[0] == '\0') {
        fprintf(stderr, "testkit-odbc: set FL_CORPUS to frostlake's engine/src/test/resources/testkit to replay "
                        "the testkit corpus\n");
        return 0;
    }
    char *suites_dir = fmt("%s/suites", corpus);
    strvec files = { 0 };
    DIR *dir = opendir(suites_dir);
    if (dir != NULL) {
        for (struct dirent *entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
            size_t n = strlen(entry->d_name);
            if (n > 5 && strcmp(entry->d_name + n - 5, ".json") == 0) {
                strvec_push(&files, xstrdup(entry->d_name));
            }
        }
        closedir(dir);
    }
    if (files.count == 0) {
        fprintf(stderr, "FL_CORPUS=%s: no suites/*.json there; point it at frostlake's "
                        "engine/src/test/resources/testkit\n", corpus);
        return 2;
    }
    qsort(files.items, files.count, sizeof(char *), by_name);

    int norm_failures = check_norm();
    if (norm_failures > 0) {
        fprintf(stderr, "%d normalization case(s) disagree with the reference\n", norm_failures);
        return 2;
    }

    const char *tmp = getenv("TMPDIR") != NULL ? getenv("TMPDIR") : "/tmp";
    char *workdir = fmt("%s/testkit-odbc-XXXXXX", tmp);
    if (mkdtemp(workdir) == NULL) {
        fprintf(stderr, "cannot create a working directory under %s\n", tmp);
        return 2;
    }
    const char *report_env = getenv("FROSTLAKE_TESTKIT_REPORT");
    char *report = report_env != NULL && report_env[0] != '\0' ? xstrdup(report_env)
                                                             : fmt("%s/testkit-odbc.tsv", tmp);

    /* Register the driver privately, the way test/smoke.sh does: nothing touches ~/.odbc*.ini. */
    const char *driver_env = getenv("FROSTLAKE_ODBC_DRIVER");
    char driver_path[PATH_MAX];
    if (realpath(driver_env != NULL ? driver_env : "build/libfrostlakeodbc.so", driver_path) == NULL) {
        fprintf(stderr, "driver library not found (build it, or set FROSTLAKE_ODBC_DRIVER)\n");
        return 2;
    }
    char *ini = fmt("%s/odbcinst.ini", workdir);
    FILE *ini_file = fopen(ini, "w");
    fprintf(ini_file, "[Frostlake ODBC Driver]\nDriver = %s\nThreading = 2\n", driver_path);
    fclose(ini_file);
    char *odbc_ini = fmt("%s/odbc.ini", workdir);
    fclose(fopen(odbc_ini, "w"));
    setenv("ODBCSYSINI", workdir, 1);
    setenv("ODBCINI", odbc_ini, 1);

    char host[256] = "localhost";
    int port = -1;
    const char *url = getenv("FROSTLAKE_URL");
    const char *classpath = getenv("FROSTLAKE_CLASSPATH");
    if (url != NULL && url[0] != '\0') {
        const char *authority = strstr(url, "://") != NULL ? strstr(url, "://") + 3 : url;
        const char *colon = strrchr(authority, ':');
        if (colon == NULL) {
            fprintf(stderr, "FROSTLAKE_URL needs a port: %s\n", url);
            return 2;
        }
        snprintf(host, sizeof(host), "%.*s", (int) (colon - authority), authority);
        port = atoi(colon + 1);
    } else if (classpath != NULL && classpath[0] != '\0') {
        port = free_port();
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
        atexit(stop_server);
        boot_server(classpath, workdir, port);
    } else {
        fprintf(stderr, "set FROSTLAKE_URL (attach) or FROSTLAKE_CLASSPATH (boot a private server)\n");
        return 2;
    }

    SQLHENV env = SQL_NULL_HENV;
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER) SQL_OV_ODBC3, 0);
    char *connection = fmt("Driver=Frostlake ODBC Driver;Server=%s;Port=%d", host, port);
    SQLHDBC dbc = connect_driver(env, connection, server_pid > 0 ? 360 : 1);
    if (dbc == SQL_NULL_HDBC) {
        return 2;
    }
    fprintf(stderr, "testkit-odbc: %s via %s, suites %s\n", connection, driver_path, suites_dir);

    FILE *tsv = fopen(report, "w");
    if (tsv == NULL) {
        fprintf(stderr, "cannot write %s\n", report);
        return 2;
    }
    fprintf(tsv, "suite\ttest\tstatus\tfailedStep\tdetail\tms\n");
    long passed = 0, failed = 0, errors = 0, skipped = 0;
    int capability_skips = 0;
    const char *filter = getenv("TESTKIT_SUITES");
    double started = now_ms();
    for (size_t f = 0; f < files.count; f++) {
        char *path = fmt("%s/%s", suites_dir, files.items[f]);
        size_t length = 0;
        char *text = read_file(path, &length);
        fl_json *suite = text != NULL ? fl_json_parse(text, length) : NULL;
        if (suite == NULL) {
            fprintf(stderr, "cannot parse %s\n", path);
            fprintf(tsv, "%s\t-\tERROR\t\tunparseable suite file\t0\n", files.items[f]);
            errors++;
            free(path);
            free(text);
            continue;
        }
        const char *suite_name = fl_json_get_string(suite, "suite");
        if (suite_name == NULL) {
            suite_name = files.items[f];
        }
        const fl_json *tests = fl_json_get(suite, "tests");
        if (wanted(filter, suite_name)) {
            for (size_t t = 0; tests != NULL && tests->kind == FL_JSON_ARRAY && t < tests->child_count; t++) {
                const fl_json *test = tests->children[t];
                const char *name = fl_json_get_string(test, "name");
                if (name == NULL) {
                    name = "(unnamed)";
                }
                const char *reason = skip_reason(test);
                if (reason != NULL) {
                    char *line = one_line(reason);
                    fprintf(tsv, "%s\t%s\tSKIP\t\t%s\t0\n", suite_name, name, line);
                    free(line);
                    skipped++;
                    continue;
                }
                double test_started = now_ms();
                verdict v = run_test(dbc, test, &capability_skips);
                long ms = (long) (now_ms() - test_started);
                char *detail = one_line(v.detail != NULL ? v.detail : "");
                if (v.step > 0) {
                    fprintf(tsv, "%s\t%s\t%s\t%ld\t%s\t%ld\n", suite_name, name, v.status, v.step, detail, ms);
                } else {
                    fprintf(tsv, "%s\t%s\t%s\t\t%s\t%ld\n", suite_name, name, v.status, detail, ms);
                }
                if (strcmp(v.status, "PASS") == 0) {
                    passed++;
                } else {
                    if (strcmp(v.status, "FAIL") == 0) {
                        failed++;
                    } else {
                        errors++;
                    }
                    if (failed + errors <= 25) {
                        fprintf(stderr, "%s %s / %s: step %ld: %.400s\n", v.status, suite_name, name, v.step,
                                detail);
                    }
                }
                free(detail);
                free(v.detail);
            }
        }
        fl_json_free(suite);
        free(text);
        free(path);
    }
    fclose(tsv);
    fprintf(stderr, "testkit-odbc: %ld passed / %ld failed / %ld errors / %ld skipped in %.1f s -> %s\n",
            passed, failed, errors, skipped, (now_ms() - started) / 1000.0, report);
    if (capability_skips > 0) {
        fprintf(stderr, "testkit-odbc missing API [odbc]: ERROR_CODE (%d check(s))\n", capability_skips);
    }

    SQLDisconnect(dbc);
    SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    SQLFreeHandle(SQL_HANDLE_ENV, env);
    strvec_free(&files);
    stop_server();
    return failed + errors == 0 ? 0 : 1;
}
