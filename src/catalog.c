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

/* Catalog functions.
 *
 * Each one queries INFORMATION_SCHEMA through the ordinary execution path and
 * then rebuilds the answer as a locally synthesised result set, because ODBC
 * dictates the exact output column names, order and types — which are not the
 * INFORMATION_SCHEMA ones.
 */

#include "fl_odbc.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- helpers -------------------------------------------------------------- */

static char *arg_to_string(SQLCHAR *text, SQLSMALLINT length) {
    if (text == NULL) {
        return NULL;
    }
    size_t n = length == SQL_NTS ? strlen((const char *) text) : (size_t) length;
    char *s = malloc(n + 1);
    if (s != NULL) {
        memcpy(s, text, n);
        s[n] = '\0';
    }
    return s;
}

static int is_empty_or_all(const char *pattern) {
    return pattern == NULL || pattern[0] == '\0' || strcmp(pattern, "%") == 0;
}

/* Append `identifier LIKE 'pattern'`, escaping the pattern into the literal.
 * Both the quote and the backslash have to be doubled: the engine's lexer
 * honours backslash escapes, so a pattern ending in one used to escape the
 * closing quote and run the literal into the rest of the statement. */
static int append_like(fl_strbuf *buf, const char *identifier, const char *pattern) {
    if (fl_strbuf_append(buf, " AND ") != 0
        || fl_strbuf_append(buf, identifier) != 0
        || fl_strbuf_append(buf, " LIKE '") != 0) {
        return -1;
    }
    for (const char *c = pattern; *c; c++) {
        if ((*c == '\'' || *c == '\\') && fl_strbuf_append_len(buf, c, 1) != 0) {
            return -1;
        }
        if (fl_strbuf_append_len(buf, c, 1) != 0) {
            return -1;
        }
    }
    return fl_strbuf_append(buf, "'");
}

/* Execute SQL on the statement's connection; returns the first result set (owned
 * by *response_out) or NULL with the diag already set. */
static fl_resultset *run_catalog_query(fl_stmt *stmt, const char *sql, fl_response **response_out) {
    char *transport_error = NULL;
    fl_response *response = fl_proto_execute(stmt->dbc, sql, &transport_error);
    if (response == NULL) {
        fl_diag_setf(stmt, "08S01", "%s", transport_error);
        free(transport_error);
        return NULL;
    }
    if (response->error_message != NULL) {
        fl_diag_setf(stmt, "42000", "%s", response->error_message);
        fl_response_free(response);
        return NULL;
    }
    if (response->set_count < 1) {
        fl_diag_set(stmt, "HY000", "Catalog query returned no result set");
        fl_response_free(response);
        return NULL;
    }
    *response_out = response;
    return &response->sets[0];
}

static const char *cell_text(const fl_resultset *set, int row, int col) {
    if (col < 0 || col >= set->column_count) {
        return NULL;
    }
    const fl_cell *cell = &set->cells[row * set->column_count + col];
    return cell->is_null ? NULL : cell->text;
}

static int column_index(const fl_resultset *set, const char *name) {
    for (int i = 0; i < set->column_count; i++) {
        if (set->columns[i].name != NULL && strcasecmp(set->columns[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void install_synth(fl_stmt *stmt, fl_resultset *synth) {
    fl_stmt_clear_results(stmt);
    stmt->synth = synth;
    stmt->current_row = -1;
    stmt->row_count = synth->row_count;
}

/* ---- SQLTables ------------------------------------------------------------- */

static const char *TABLES_COLUMNS[5] =
    { "TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS" };

static fl_resultset *tables_shape(int row_capacity) {
    fl_resultset *synth = fl_synth_new(5, row_capacity);
    if (synth == NULL) {
        return NULL;
    }
    for (int i = 0; i < 5; i++) {
        fl_synth_column(synth, i, TABLES_COLUMNS[i], SQL_VARCHAR);
    }
    return synth;
}

/* "'TABLE','VIEW'" / "TABLE,VIEW" -> does it name `wanted`? */
static int type_list_contains(const char *list, const char *wanted) {
    if (is_empty_or_all(list)) {
        return 1;
    }
    const char *cur = list;
    size_t wanted_length = strlen(wanted);
    while (*cur != '\0') {
        while (*cur == ' ' || *cur == ',' || *cur == '\'') {
            cur++;
        }
        const char *start = cur;
        while (*cur != '\0' && *cur != ',' && *cur != '\'') {
            cur++;
        }
        const char *end = cur;
        while (end > start && end[-1] == ' ') {
            end--;
        }
        if ((size_t) (end - start) == wanted_length
            && strncasecmp(start, wanted, wanted_length) == 0) {
            return 1;
        }
    }
    return 0;
}

SQLRETURN SQL_API SQLTables(SQLHSTMT StatementHandle,
                            SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
                            SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
                            SQLCHAR *TableName, SQLSMALLINT NameLength3,
                            SQLCHAR *TableType, SQLSMALLINT NameLength4) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);

    char *catalog = arg_to_string(CatalogName, NameLength1);
    char *schema = arg_to_string(SchemaName, NameLength2);
    char *table = arg_to_string(TableName, NameLength3);
    char *types = arg_to_string(TableType, NameLength4);
    SQLRETURN rc = SQL_ERROR;

    /* SQL_ALL_CATALOGS enumeration: catalog="%", schema and table empty */
    if (catalog != NULL && strcmp(catalog, "%") == 0
        && schema != NULL && schema[0] == '\0'
        && table != NULL && table[0] == '\0') {
        fl_response *response = NULL;
        fl_resultset *shown = run_catalog_query(stmt, "SHOW DATABASES", &response);
        if (shown == NULL) {
            goto done;
        }
        int name_column = column_index(shown, "name");
        fl_resultset *synth = tables_shape(shown->row_count);
        if (synth == NULL) {
            fl_response_free(response);
            rc = fl_diag_set(stmt, "HY001", "Out of memory");
            goto done;
        }
        for (int r = 0; r < shown->row_count; r++) {
            int row = fl_synth_add_row(synth);
            const char *name = cell_text(shown, r, name_column);
            fl_synth_text(synth, row, 0, name != NULL ? name : "");
            for (int c = 1; c < 5; c++) {
                fl_synth_null(synth, row, c);
            }
        }
        fl_response_free(response);
        install_synth(stmt, synth);
        rc = SQL_SUCCESS;
        goto done;
    }

    /* SQL_ALL_TABLE_TYPES enumeration: type="%", the rest empty */
    if (types != NULL && strcmp(types, "%") == 0
        && is_empty_or_all(catalog) && is_empty_or_all(schema) && is_empty_or_all(table)
        && (catalog == NULL || catalog[0] == '\0')) {
        fl_resultset *synth = tables_shape(2);
        if (synth == NULL) {
            rc = fl_diag_set(stmt, "HY001", "Out of memory");
            goto done;
        }
        static const char *kinds[2] = { "TABLE", "VIEW" };
        for (int i = 0; i < 2; i++) {
            int row = fl_synth_add_row(synth);
            for (int c = 0; c < 5; c++) {
                if (c == 3) {
                    fl_synth_text(synth, row, c, kinds[i]);
                } else {
                    fl_synth_null(synth, row, c);
                }
            }
        }
        install_synth(stmt, synth);
        rc = SQL_SUCCESS;
        goto done;
    }

    /* the general listing */
    fl_strbuf sql;
    fl_strbuf_init(&sql);
    int failed = fl_strbuf_append(&sql, "SELECT table_catalog, table_schema, table_name, table_type FROM ");
    if (!is_empty_or_all(catalog) && strchr(catalog, '%') == NULL && strchr(catalog, '_') == NULL) {
        failed = failed || fl_strbuf_append_ident(&sql, catalog);
        failed = failed || fl_strbuf_append(&sql, ".");
    }
    failed = failed || fl_strbuf_append(&sql, "information_schema.tables WHERE 1=1");
    if (!is_empty_or_all(schema)) {
        failed = failed || append_like(&sql, "table_schema", schema);
    }
    if (!is_empty_or_all(table)) {
        failed = failed || append_like(&sql, "table_name", table);
    }
    failed = failed || fl_strbuf_append(&sql, " ORDER BY table_schema, table_name");
    if (failed) {
        fl_strbuf_free(&sql);
        rc = fl_diag_set(stmt, "HY001", "Out of memory");
        goto done;
    }

    fl_response *response = NULL;
    fl_resultset *listed = run_catalog_query(stmt, sql.data, &response);
    fl_strbuf_free(&sql);
    if (listed == NULL) {
        goto done;
    }
    int cat_col = column_index(listed, "table_catalog");
    int schema_col = column_index(listed, "table_schema");
    int name_col = column_index(listed, "table_name");
    int type_col = column_index(listed, "table_type");

    fl_resultset *synth = tables_shape(listed->row_count);
    if (synth == NULL) {
        fl_response_free(response);
        rc = fl_diag_set(stmt, "HY001", "Out of memory");
        goto done;
    }
    for (int r = 0; r < listed->row_count; r++) {
        const char *raw_type = cell_text(listed, r, type_col);
        const char *mapped = raw_type != NULL && strcasecmp(raw_type, "BASE TABLE") == 0
            ? "TABLE" : raw_type;
        if (mapped != NULL && !type_list_contains(types, mapped)) {
            continue;
        }
        int row = fl_synth_add_row(synth);
        const char *cat = cell_text(listed, r, cat_col);
        const char *sch = cell_text(listed, r, schema_col);
        const char *name = cell_text(listed, r, name_col);
        if (cat != NULL) fl_synth_text(synth, row, 0, cat); else fl_synth_null(synth, row, 0);
        if (sch != NULL) fl_synth_text(synth, row, 1, sch); else fl_synth_null(synth, row, 1);
        fl_synth_text(synth, row, 2, name != NULL ? name : "");
        fl_synth_text(synth, row, 3, mapped != NULL ? mapped : "TABLE");
        fl_synth_null(synth, row, 4);
    }
    fl_response_free(response);
    install_synth(stmt, synth);
    rc = SQL_SUCCESS;

done:
    free(catalog);
    free(schema);
    free(table);
    free(types);
    return rc;
}

/* ---- SQLColumns ------------------------------------------------------------- */

static const char *COLUMNS_COLUMNS[18] = {
    "TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "DATA_TYPE",
    "TYPE_NAME", "COLUMN_SIZE", "BUFFER_LENGTH", "DECIMAL_DIGITS", "NUM_PREC_RADIX",
    "NULLABLE", "REMARKS", "COLUMN_DEF", "SQL_DATA_TYPE", "SQL_DATETIME_SUB",
    "CHAR_OCTET_LENGTH", "ORDINAL_POSITION", "IS_NULLABLE"
};

SQLRETURN SQL_API SQLColumns(SQLHSTMT StatementHandle,
                             SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
                             SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
                             SQLCHAR *TableName, SQLSMALLINT NameLength3,
                             SQLCHAR *ColumnName, SQLSMALLINT NameLength4) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);

    char *catalog = arg_to_string(CatalogName, NameLength1);
    char *schema = arg_to_string(SchemaName, NameLength2);
    char *table = arg_to_string(TableName, NameLength3);
    char *column = arg_to_string(ColumnName, NameLength4);
    SQLRETURN rc = SQL_ERROR;

    fl_strbuf sql;
    fl_strbuf_init(&sql);
    int failed = fl_strbuf_append(&sql,
        "SELECT table_catalog, table_schema, table_name, column_name, data_type,"
        " character_maximum_length, numeric_precision, numeric_scale, is_nullable,"
        " ordinal_position, column_default FROM ");
    if (!is_empty_or_all(catalog) && strchr(catalog, '%') == NULL && strchr(catalog, '_') == NULL) {
        failed = failed || fl_strbuf_append_ident(&sql, catalog);
        failed = failed || fl_strbuf_append(&sql, ".");
    }
    failed = failed || fl_strbuf_append(&sql, "information_schema.columns WHERE 1=1");
    if (!is_empty_or_all(schema)) {
        failed = failed || append_like(&sql, "table_schema", schema);
    }
    if (!is_empty_or_all(table)) {
        failed = failed || append_like(&sql, "table_name", table);
    }
    if (!is_empty_or_all(column)) {
        failed = failed || append_like(&sql, "column_name", column);
    }
    failed = failed || fl_strbuf_append(&sql, " ORDER BY table_schema, table_name, ordinal_position");
    if (failed) {
        fl_strbuf_free(&sql);
        rc = fl_diag_set(stmt, "HY001", "Out of memory");
        goto done;
    }

    fl_response *response = NULL;
    fl_resultset *listed = run_catalog_query(stmt, sql.data, &response);
    fl_strbuf_free(&sql);
    if (listed == NULL) {
        goto done;
    }

    int cat_col = column_index(listed, "table_catalog");
    int schema_col = column_index(listed, "table_schema");
    int table_col = column_index(listed, "table_name");
    int name_col = column_index(listed, "column_name");
    int type_col = column_index(listed, "data_type");
    int charlen_col = column_index(listed, "character_maximum_length");
    int precision_col = column_index(listed, "numeric_precision");
    int scale_col = column_index(listed, "numeric_scale");
    int nullable_col = column_index(listed, "is_nullable");
    int position_col = column_index(listed, "ordinal_position");
    int default_col = column_index(listed, "column_default");

    fl_resultset *synth = fl_synth_new(18, listed->row_count > 0 ? listed->row_count : 1);
    if (synth == NULL) {
        fl_response_free(response);
        rc = fl_diag_set(stmt, "HY001", "Out of memory");
        goto done;
    }
    for (int i = 0; i < 18; i++) {
        SQLSMALLINT type = SQL_VARCHAR;
        if (i == 4 || i == 10 || i == 13 || i == 14) {
            type = SQL_SMALLINT; /* DATA_TYPE, NULLABLE, SQL_DATA_TYPE, SQL_DATETIME_SUB */
        } else if (i == 6 || i == 7 || i == 8 || i == 9 || i == 15 || i == 16) {
            type = SQL_INTEGER;
        }
        fl_synth_column(synth, i, COLUMNS_COLUMNS[i], type);
    }

    for (int r = 0; r < listed->row_count; r++) {
        int row = fl_synth_add_row(synth);
        const char *type_name = cell_text(listed, r, type_col);
        const char *scale_text = cell_text(listed, r, scale_col);
        long scale = scale_text != NULL ? strtol(scale_text, NULL, 10) : 0;
        SQLSMALLINT sql_type = fl_sql_type_for(type_name, (SQLINTEGER) scale);

        const char *charlen_text = cell_text(listed, r, charlen_col);
        const char *precision_text = cell_text(listed, r, precision_col);
        long column_size = charlen_text != NULL ? strtol(charlen_text, NULL, 10)
            : precision_text != NULL ? strtol(precision_text, NULL, 10)
            : 0;
        if (column_size == 0) {
            fl_column probe;
            memset(&probe, 0, sizeof(probe));
            probe.sql_type = sql_type;
            SQLULEN fallback = 0;
            fl_type_display_size(&probe, &fallback, NULL);
            column_size = (long) fallback;
        }

        const char *nullable_text = cell_text(listed, r, nullable_col);
        int nullable = nullable_text == NULL || strcasecmp(nullable_text, "YES") == 0;

        const char *cat = cell_text(listed, r, cat_col);
        const char *sch = cell_text(listed, r, schema_col);
        const char *tab = cell_text(listed, r, table_col);
        const char *nam = cell_text(listed, r, name_col);
        const char *dfl = cell_text(listed, r, default_col);
        const char *pos = cell_text(listed, r, position_col);

        if (cat != NULL) fl_synth_text(synth, row, 0, cat); else fl_synth_null(synth, row, 0);
        if (sch != NULL) fl_synth_text(synth, row, 1, sch); else fl_synth_null(synth, row, 1);
        fl_synth_text(synth, row, 2, tab != NULL ? tab : "");
        fl_synth_text(synth, row, 3, nam != NULL ? nam : "");
        fl_synth_int(synth, row, 4, sql_type);
        fl_synth_text(synth, row, 5, type_name != NULL ? type_name : "VARCHAR");
        fl_synth_int(synth, row, 6, column_size);
        fl_synth_int(synth, row, 7, column_size);
        if (scale_text != NULL) fl_synth_int(synth, row, 8, scale); else fl_synth_null(synth, row, 8);
        fl_synth_int(synth, row, 9, sql_type == SQL_DOUBLE ? 2 : 10);
        fl_synth_int(synth, row, 10, nullable ? SQL_NULLABLE : SQL_NO_NULLS);
        fl_synth_null(synth, row, 11);
        if (dfl != NULL) fl_synth_text(synth, row, 12, dfl); else fl_synth_null(synth, row, 12);
        fl_synth_int(synth, row, 13, sql_type == SQL_TYPE_DATE || sql_type == SQL_TYPE_TIME
            || sql_type == SQL_TYPE_TIMESTAMP ? SQL_DATETIME : sql_type);
        if (sql_type == SQL_TYPE_DATE) fl_synth_int(synth, row, 14, SQL_CODE_DATE);
        else if (sql_type == SQL_TYPE_TIME) fl_synth_int(synth, row, 14, SQL_CODE_TIME);
        else if (sql_type == SQL_TYPE_TIMESTAMP) fl_synth_int(synth, row, 14, SQL_CODE_TIMESTAMP);
        else fl_synth_null(synth, row, 14);
        fl_synth_int(synth, row, 15, column_size);
        fl_synth_int(synth, row, 16, pos != NULL ? strtol(pos, NULL, 10) : row + 1);
        fl_synth_text(synth, row, 17, nullable ? "YES" : "NO");
    }
    fl_response_free(response);
    install_synth(stmt, synth);
    rc = SQL_SUCCESS;

done:
    free(catalog);
    free(schema);
    free(table);
    free(column);
    return rc;
}

/* ---- SQLGetTypeInfo ---------------------------------------------------------- */

typedef struct type_row {
    const char *name;
    SQLSMALLINT data_type;
    long column_size;
    const char *prefix;
    const char *suffix;
    const char *create_params;
    SQLSMALLINT min_scale;
    SQLSMALLINT max_scale;
} type_row;

SQLRETURN SQL_API SQLGetTypeInfo(SQLHSTMT StatementHandle, SQLSMALLINT DataType) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);

    static const type_row types[] = {
        { "VARCHAR", SQL_VARCHAR, 16777216, "'", "'", "max length", 0, 0 },
        { "NUMBER", SQL_DECIMAL, 38, NULL, NULL, "precision,scale", 0, 37 },
        { "NUMBER", SQL_BIGINT, 19, NULL, NULL, NULL, 0, 0 },
        { "FLOAT", SQL_DOUBLE, 15, NULL, NULL, NULL, 0, 0 },
        { "BOOLEAN", SQL_BIT, 1, NULL, NULL, NULL, 0, 0 },
        { "DATE", SQL_TYPE_DATE, 10, "'", "'", NULL, 0, 0 },
        { "TIME", SQL_TYPE_TIME, 8, "'", "'", NULL, 0, 9 },
        { "TIMESTAMP_NTZ", SQL_TYPE_TIMESTAMP, 29, "'", "'", NULL, 0, 9 },
        { "BINARY", SQL_VARBINARY, 8388608, "X'", "'", "max length", 0, 0 },
        { "VARIANT", SQL_VARCHAR, 16777216, NULL, NULL, NULL, 0, 0 },
    };
    static const char *columns[19] = {
        "TYPE_NAME", "DATA_TYPE", "COLUMN_SIZE", "LITERAL_PREFIX", "LITERAL_SUFFIX",
        "CREATE_PARAMS", "NULLABLE", "CASE_SENSITIVE", "SEARCHABLE", "UNSIGNED_ATTRIBUTE",
        "FIXED_PREC_SCALE", "AUTO_UNIQUE_VALUE", "LOCAL_TYPE_NAME", "MINIMUM_SCALE",
        "MAXIMUM_SCALE", "SQL_DATA_TYPE", "SQL_DATETIME_SUB", "NUM_PREC_RADIX",
        "INTERVAL_PRECISION"
    };

    int count = (int) (sizeof(types) / sizeof(types[0]));
    fl_resultset *synth = fl_synth_new(19, count);
    if (synth == NULL) {
        return fl_diag_set(stmt, "HY001", "Out of memory");
    }
    for (int i = 0; i < 19; i++) {
        SQLSMALLINT type = SQL_VARCHAR;
        if (i == 1 || i == 6 || i == 7 || i == 8 || i == 9 || i == 10 || i == 11
            || i == 13 || i == 14 || i == 15 || i == 16 || i == 18) {
            type = SQL_SMALLINT;
        } else if (i == 2 || i == 17) {
            type = SQL_INTEGER;
        }
        fl_synth_column(synth, i, columns[i], type);
    }

    for (int i = 0; i < count; i++) {
        const type_row *t = &types[i];
        if (DataType != SQL_ALL_TYPES && DataType != t->data_type) {
            continue;
        }
        int row = fl_synth_add_row(synth);
        fl_synth_text(synth, row, 0, t->name);
        fl_synth_int(synth, row, 1, t->data_type);
        fl_synth_int(synth, row, 2, t->column_size);
        if (t->prefix != NULL) fl_synth_text(synth, row, 3, t->prefix); else fl_synth_null(synth, row, 3);
        if (t->suffix != NULL) fl_synth_text(synth, row, 4, t->suffix); else fl_synth_null(synth, row, 4);
        if (t->create_params != NULL) fl_synth_text(synth, row, 5, t->create_params);
        else fl_synth_null(synth, row, 5);
        fl_synth_int(synth, row, 6, SQL_NULLABLE);
        fl_synth_int(synth, row, 7, t->data_type == SQL_VARCHAR ? SQL_TRUE : SQL_FALSE);
        fl_synth_int(synth, row, 8, SQL_SEARCHABLE);
        fl_synth_int(synth, row, 9, SQL_FALSE);
        fl_synth_int(synth, row, 10, SQL_FALSE);
        fl_synth_int(synth, row, 11, SQL_FALSE);
        fl_synth_text(synth, row, 12, t->name);
        fl_synth_int(synth, row, 13, t->min_scale);
        fl_synth_int(synth, row, 14, t->max_scale);
        fl_synth_int(synth, row, 15, t->data_type == SQL_TYPE_DATE || t->data_type == SQL_TYPE_TIME
            || t->data_type == SQL_TYPE_TIMESTAMP ? SQL_DATETIME : t->data_type);
        if (t->data_type == SQL_TYPE_DATE) fl_synth_int(synth, row, 16, SQL_CODE_DATE);
        else if (t->data_type == SQL_TYPE_TIME) fl_synth_int(synth, row, 16, SQL_CODE_TIME);
        else if (t->data_type == SQL_TYPE_TIMESTAMP) fl_synth_int(synth, row, 16, SQL_CODE_TIMESTAMP);
        else fl_synth_null(synth, row, 16);
        fl_synth_int(synth, row, 17, t->data_type == SQL_DOUBLE ? 2 : 10);
        fl_synth_null(synth, row, 18);
    }
    install_synth(stmt, synth);
    return SQL_SUCCESS;
}

/* ---- intentionally empty catalog answers ------------------------------------ */

static SQLRETURN empty_result(fl_stmt *stmt, int column_count, const char **names) {
    fl_resultset *synth = fl_synth_new(column_count, 1);
    if (synth == NULL) {
        return fl_diag_set(stmt, "HY001", "Out of memory");
    }
    for (int i = 0; i < column_count; i++) {
        fl_synth_column(synth, i, names[i], SQL_VARCHAR);
    }
    install_synth(stmt, synth);
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLStatistics(SQLHSTMT StatementHandle,
                                SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
                                SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
                                SQLCHAR *TableName, SQLSMALLINT NameLength3,
                                SQLUSMALLINT Unique, SQLUSMALLINT Reserved) {
    (void) CatalogName; (void) NameLength1; (void) SchemaName; (void) NameLength2;
    (void) TableName; (void) NameLength3; (void) Unique; (void) Reserved;
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    /* the engine keeps no indexes — an empty answer is the truthful one */
    static const char *names[13] = {
        "TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "NON_UNIQUE", "INDEX_QUALIFIER",
        "INDEX_NAME", "TYPE", "ORDINAL_POSITION", "COLUMN_NAME", "ASC_OR_DESC",
        "CARDINALITY", "PAGES", "FILTER_CONDITION"
    };
    return empty_result(stmt, 13, names);
}

SQLRETURN SQL_API SQLSpecialColumns(SQLHSTMT StatementHandle, SQLUSMALLINT IdentifierType,
                                    SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
                                    SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
                                    SQLCHAR *TableName, SQLSMALLINT NameLength3,
                                    SQLUSMALLINT Scope, SQLUSMALLINT Nullable) {
    (void) IdentifierType; (void) CatalogName; (void) NameLength1; (void) SchemaName;
    (void) NameLength2; (void) TableName; (void) NameLength3; (void) Scope; (void) Nullable;
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    static const char *names[8] = {
        "SCOPE", "COLUMN_NAME", "DATA_TYPE", "TYPE_NAME", "COLUMN_SIZE",
        "BUFFER_LENGTH", "DECIMAL_DIGITS", "PSEUDO_COLUMN"
    };
    return empty_result(stmt, 8, names);
}

SQLRETURN SQL_API SQLPrimaryKeys(SQLHSTMT StatementHandle,
                                 SQLCHAR *CatalogName, SQLSMALLINT NameLength1,
                                 SQLCHAR *SchemaName, SQLSMALLINT NameLength2,
                                 SQLCHAR *TableName, SQLSMALLINT NameLength3) {
    (void) CatalogName; (void) NameLength1; (void) SchemaName; (void) NameLength2;
    (void) TableName; (void) NameLength3;
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    static const char *names[6] = {
        "TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "KEY_SEQ", "PK_NAME"
    };
    return empty_result(stmt, 6, names);
}
