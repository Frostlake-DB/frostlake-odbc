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

/* Result description, fetching and data conversion.
 *
 * Cells live as UTF-8 wire text; every SQLGetData / bound-column conversion
 * starts from that text. Numeric targets parse it (rejecting garbage with
 * SQLSTATE 22018), temporal targets parse the Snowflake-shaped renderings the
 * server emits ("YYYY-MM-DD HH:MM:SS.fff [±offset]"), and SQL_C_BINARY decodes
 * the uppercase hex a BINARY cell crosses the wire as.
 */

#include "fl_odbc.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- describing ------------------------------------------------------------ */

SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT StatementHandle, SQLSMALLINT *ColumnCountPtr) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    fl_resultset *set = fl_stmt_current_set(stmt);
    if (ColumnCountPtr != NULL) {
        *ColumnCountPtr = set != NULL ? (SQLSMALLINT) set->column_count : 0;
    }
    return SQL_SUCCESS;
}

static fl_column *column_at(fl_stmt *stmt, SQLUSMALLINT number) {
    fl_resultset *set = fl_stmt_current_set(stmt);
    if (set == NULL || number < 1 || number > (SQLUSMALLINT) set->column_count) {
        return NULL;
    }
    return &set->columns[number - 1];
}

SQLRETURN SQL_API SQLDescribeCol(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                                 SQLCHAR *ColumnName, SQLSMALLINT BufferLength,
                                 SQLSMALLINT *NameLengthPtr, SQLSMALLINT *DataTypePtr,
                                 SQLULEN *ColumnSizePtr, SQLSMALLINT *DecimalDigitsPtr,
                                 SQLSMALLINT *NullablePtr) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    fl_column *column = column_at(stmt, ColumnNumber);
    if (column == NULL) {
        return fl_diag_set(stmt, "07009", "Invalid column number");
    }
    SQLRETURN rc = fl_copy_string(stmt, column->name, ColumnName, BufferLength, NameLengthPtr);
    if (DataTypePtr != NULL) {
        *DataTypePtr = column->sql_type;
    }
    SQLULEN size = 0;
    SQLSMALLINT digits = 0;
    fl_type_display_size(column, &size, &digits);
    if (ColumnSizePtr != NULL) {
        *ColumnSizePtr = size;
    }
    if (DecimalDigitsPtr != NULL) {
        *DecimalDigitsPtr = digits;
    }
    if (NullablePtr != NULL) {
        *NullablePtr = SQL_NULLABLE_UNKNOWN;
    }
    return rc;
}

SQLRETURN SQL_API SQLColAttribute(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                                  SQLUSMALLINT FieldIdentifier, SQLPOINTER CharacterAttributePtr,
                                  SQLSMALLINT BufferLength, SQLSMALLINT *StringLengthPtr,
                                  SQLLEN *NumericAttributePtr) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);

    if (FieldIdentifier == SQL_DESC_COUNT || FieldIdentifier == SQL_COLUMN_COUNT) {
        fl_resultset *set = fl_stmt_current_set(stmt);
        if (NumericAttributePtr != NULL) {
            *NumericAttributePtr = set != NULL ? set->column_count : 0;
        }
        return SQL_SUCCESS;
    }

    fl_column *column = column_at(stmt, ColumnNumber);
    if (column == NULL) {
        return fl_diag_set(stmt, "07009", "Invalid column number");
    }

    SQLULEN size = 0;
    SQLSMALLINT digits = 0;
    fl_type_display_size(column, &size, &digits);

    switch (FieldIdentifier) {
        case SQL_DESC_NAME:
        case SQL_DESC_LABEL:
        case SQL_DESC_BASE_COLUMN_NAME:
        case SQL_COLUMN_NAME:
            return fl_copy_string(stmt, column->name, (SQLCHAR *) CharacterAttributePtr,
                                  BufferLength, StringLengthPtr);
        case SQL_DESC_TYPE_NAME:
            return fl_copy_string(stmt, column->type_name != NULL ? column->type_name : "VARCHAR",
                                  (SQLCHAR *) CharacterAttributePtr, BufferLength, StringLengthPtr);
        case SQL_DESC_BASE_TABLE_NAME:
        case SQL_DESC_TABLE_NAME:
        case SQL_DESC_SCHEMA_NAME:
        case SQL_DESC_CATALOG_NAME:
        case SQL_DESC_LITERAL_PREFIX:
        case SQL_DESC_LITERAL_SUFFIX:
        case SQL_DESC_LOCAL_TYPE_NAME:
            return fl_copy_string(stmt, "", (SQLCHAR *) CharacterAttributePtr,
                                  BufferLength, StringLengthPtr);
        case SQL_DESC_TYPE:
        case SQL_DESC_CONCISE_TYPE: /* == the ODBC 2.x SQL_COLUMN_TYPE */
            if (NumericAttributePtr != NULL) {
                /* SQL_DESC_TYPE reports the verbose datetime type */
                if (FieldIdentifier == SQL_DESC_TYPE
                    && (column->sql_type == SQL_TYPE_DATE || column->sql_type == SQL_TYPE_TIME
                        || column->sql_type == SQL_TYPE_TIMESTAMP)) {
                    *NumericAttributePtr = SQL_DATETIME;
                } else {
                    *NumericAttributePtr = column->sql_type;
                }
            }
            return SQL_SUCCESS;
        case SQL_DESC_LENGTH:
        case SQL_COLUMN_LENGTH:
        case SQL_DESC_DISPLAY_SIZE:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = (SQLLEN) size;
            }
            return SQL_SUCCESS;
        case SQL_DESC_OCTET_LENGTH:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = (SQLLEN) size;
            }
            return SQL_SUCCESS;
        case SQL_DESC_PRECISION:
        case SQL_COLUMN_PRECISION:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = (SQLLEN) size;
            }
            return SQL_SUCCESS;
        case SQL_DESC_SCALE:
        case SQL_COLUMN_SCALE:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = digits;
            }
            return SQL_SUCCESS;
        case SQL_DESC_NULLABLE:
        case SQL_COLUMN_NULLABLE:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = SQL_NULLABLE_UNKNOWN;
            }
            return SQL_SUCCESS;
        case SQL_DESC_UNSIGNED:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = SQL_FALSE;
            }
            return SQL_SUCCESS;
        case SQL_DESC_AUTO_UNIQUE_VALUE:
        case SQL_DESC_CASE_SENSITIVE:
        case SQL_DESC_FIXED_PREC_SCALE:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = SQL_FALSE;
            }
            return SQL_SUCCESS;
        case SQL_DESC_SEARCHABLE:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = SQL_PRED_SEARCHABLE;
            }
            return SQL_SUCCESS;
        case SQL_DESC_UPDATABLE:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = SQL_ATTR_READONLY;
            }
            return SQL_SUCCESS;
        case SQL_DESC_UNNAMED:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = column->name != NULL && column->name[0] != '\0'
                    ? SQL_NAMED : SQL_UNNAMED;
            }
            return SQL_SUCCESS;
        case SQL_DESC_NUM_PREC_RADIX:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = column->sql_type == SQL_DOUBLE ? 2
                    : (column->sql_type == SQL_DECIMAL || column->sql_type == SQL_BIGINT
                       || column->sql_type == SQL_INTEGER) ? 10 : 0;
            }
            return SQL_SUCCESS;
        default:
            if (NumericAttributePtr != NULL) {
                *NumericAttributePtr = 0;
            }
            return SQL_SUCCESS;
    }
}

/* ---- conversions ------------------------------------------------------------ */

static SQLSMALLINT default_c_type(const fl_column *column) {
    switch (column->sql_type) {
        case SQL_BIGINT: return SQL_C_SBIGINT;
        case SQL_INTEGER: return SQL_C_SLONG;
        case SQL_SMALLINT: return SQL_C_SSHORT;
        case SQL_TINYINT: return SQL_C_STINYINT;
        case SQL_DOUBLE: return SQL_C_DOUBLE;
        case SQL_BIT: return SQL_C_BIT;
        case SQL_TYPE_DATE: return SQL_C_TYPE_DATE;
        case SQL_TYPE_TIME: return SQL_C_TYPE_TIME;
        case SQL_TYPE_TIMESTAMP: return SQL_C_TYPE_TIMESTAMP;
        case SQL_VARBINARY: return SQL_C_BINARY;
        default: return SQL_C_CHAR;
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse "YYYY-MM-DD" from the front of text. Returns chars consumed or -1. */
static int parse_date_part(const char *text, SQL_DATE_STRUCT *date) {
    int year, month, day;
    if (sscanf(text, "%4d-%2d-%2d", &year, &month, &day) != 3) {
        return -1;
    }
    date->year = (SQLSMALLINT) year;
    date->month = (SQLUSMALLINT) month;
    date->day = (SQLUSMALLINT) day;
    return 10;
}

/* Parse "HH:MM:SS[.fraction]" from the front of text; fraction in nanoseconds. */
static int parse_time_part(const char *text, SQL_TIME_STRUCT *time, SQLUINTEGER *fraction) {
    int hour, minute, second;
    if (sscanf(text, "%2d:%2d:%2d", &hour, &minute, &second) != 3) {
        return -1;
    }
    time->hour = (SQLUSMALLINT) hour;
    time->minute = (SQLUSMALLINT) minute;
    time->second = (SQLUSMALLINT) second;
    int consumed = 8;
    if (fraction != NULL) {
        *fraction = 0;
        if (text[consumed] == '.') {
            consumed++;
            unsigned long value = 0;
            int digits = 0;
            while (digits < 9 && text[consumed] >= '0' && text[consumed] <= '9') {
                value = value * 10 + (unsigned long) (text[consumed] - '0');
                digits++;
                consumed++;
            }
            while (text[consumed] >= '0' && text[consumed] <= '9') {
                consumed++; /* beyond nanoseconds: ignore */
            }
            for (; digits < 9; digits++) {
                value *= 10;
            }
            *fraction = (SQLUINTEGER) value;
        }
    }
    return consumed;
}

/* The engine renders timestamps "YYYY-MM-DD HH:MM:SS[.fff][ ±offset]"; the
 * offset (an LTZ/TZ rendering) is parsed past but not represented in the
 * offset-less ODBC timestamp struct. */
static int parse_timestamp(const char *text, SQL_TIMESTAMP_STRUCT *ts) {
    SQL_DATE_STRUCT date;
    int date_consumed = parse_date_part(text, &date);
    if (date_consumed < 0) {
        return -1;
    }
    ts->year = date.year;
    ts->month = date.month;
    ts->day = date.day;
    ts->hour = 0;
    ts->minute = 0;
    ts->second = 0;
    ts->fraction = 0;
    const char *rest = text + date_consumed;
    if (*rest == ' ' || *rest == 'T') {
        SQL_TIME_STRUCT time;
        SQLUINTEGER fraction = 0;
        if (parse_time_part(rest + 1, &time, &fraction) < 0) {
            return -1;
        }
        ts->hour = time.hour;
        ts->minute = time.minute;
        ts->second = time.second;
        ts->fraction = fraction;
    }
    return 0;
}

/* Numeric text -> long long, tolerating a decimal tail ("42.00" -> 42).
 * Returns 0 on success, -1 when the text is not a number at all, and -2 when it
 * is a number the target cannot hold — the caller reports those as 22018 and
 * 22003 respectively, which conflating them into one code did not allow. */
static int parse_integer(const char *text, long long *out) {
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 10);
    if (end == text) {
        return -1;
    }
    if (errno == ERANGE) {
        return -2;
    }
    if (*end == '.') {
        end++;
        while (*end >= '0' && *end <= '9') {
            end++;
        }
    }
    while (*end == ' ') {
        end++;
    }
    if (*end != '\0') {
        return -1;
    }
    *out = value;
    return 0;
}

static SQLRETURN out_of_range(fl_stmt *stmt) {
    return fl_diag_set(stmt, "22003", "Numeric value out of range");
}

static SQLRETURN invalid_number(fl_stmt *stmt, const char *text) {
    return fl_diag_setf(stmt, "22018", "Invalid character value for cast: '%s'", text);
}

/* Core conversion: one cell into one C buffer. `offset` supports chunked
 * SQL_C_CHAR / SQL_C_BINARY retrieval and is advanced on partial reads.
 *
 * Once a value has been delivered in full the offset is parked at total + 1,
 * one past any real position, and that is what the next call reads as
 * SQL_NO_DATA. A plain "offset == total" cannot say it: an empty cell is at
 * total from the very first call, so the caller's usual
 * `while (SQLGetData(...) != SQL_NO_DATA)` loop never ended. */
static SQLRETURN convert_cell(fl_stmt *stmt, const fl_column *column, const fl_cell *cell,
                              SQLSMALLINT c_type, SQLPOINTER buffer, SQLLEN buffer_length,
                              SQLLEN *str_len_or_ind, size_t *offset) {
    if (cell->is_null) {
        if (str_len_or_ind == NULL) {
            return fl_diag_set(stmt, "22002", "Indicator variable required but not supplied");
        }
        *str_len_or_ind = SQL_NULL_DATA;
        return SQL_SUCCESS;
    }
    const char *text = cell->text != NULL ? cell->text : "";
    if (c_type == SQL_C_DEFAULT) {
        c_type = default_c_type(column);
    }

    switch (c_type) {
        case SQL_C_CHAR: {
            size_t total = strlen(text);
            size_t from = offset != NULL ? *offset : 0;
            if (from > total) {
                return SQL_NO_DATA;
            }
            size_t remaining = total - from;
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = (SQLLEN) remaining;
            }
            if (buffer == NULL || buffer_length <= 0) {
                return SQL_SUCCESS;
            }
            size_t room = (size_t) buffer_length - 1;
            size_t copy = remaining < room ? remaining : room;
            if (copy < remaining) {
                /* Cells are UTF-8, so stop on a character boundary rather than
                 * handing back a dangling lead byte. Back up over continuation
                 * bytes (10xxxxxx) at the cut. If even one character will not
                 * fit, the raw split stands — a zero-length chunk would never
                 * advance the offset and the read could not finish. */
                size_t whole = copy;
                while (whole > 0 && ((unsigned char) text[from + whole] & 0xC0) == 0x80) {
                    whole--;
                }
                if (whole > 0) {
                    copy = whole;
                }
            }
            memcpy(buffer, text + from, copy);
            ((char *) buffer)[copy] = '\0';
            if (offset != NULL) {
                *offset = copy < remaining ? from + copy : total + 1;
            }
            if (copy < remaining) {
                fl_diag_set(stmt, "01004", "String data, right truncated");
                return SQL_SUCCESS_WITH_INFO;
            }
            return SQL_SUCCESS;
        }
        case SQL_C_BINARY: {
            /* BINARY columns cross as hex text; decode. Other columns hand
             * their raw bytes through. */
            if (column->sql_type == SQL_VARBINARY || column->sql_type == SQL_BINARY) {
                size_t hex_length = strlen(text);
                size_t total = hex_length / 2;
                size_t from = offset != NULL ? *offset : 0;
                if (from > total) {
                    return SQL_NO_DATA;
                }
                size_t remaining = total - from;
                if (str_len_or_ind != NULL) {
                    *str_len_or_ind = (SQLLEN) remaining;
                }
                if (buffer == NULL || buffer_length <= 0) {
                    return SQL_SUCCESS;
                }
                size_t copy = remaining < (size_t) buffer_length ? remaining : (size_t) buffer_length;
                for (size_t i = 0; i < copy; i++) {
                    int high = hex_nibble(text[(from + i) * 2]);
                    int low = hex_nibble(text[(from + i) * 2 + 1]);
                    if (high < 0 || low < 0) {
                        return fl_diag_set(stmt, "22018", "BINARY cell is not valid hex");
                    }
                    ((unsigned char *) buffer)[i] = (unsigned char) ((high << 4) | low);
                }
                if (offset != NULL) {
                    *offset = copy < remaining ? from + copy : total + 1;
                }
                if (copy < remaining) {
                    fl_diag_set(stmt, "01004", "String data, right truncated");
                    return SQL_SUCCESS_WITH_INFO;
                }
                return SQL_SUCCESS;
            }
            size_t total = strlen(text);
            size_t from = offset != NULL ? *offset : 0;
            if (from > total) {
                return SQL_NO_DATA;
            }
            size_t remaining = total - from;
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = (SQLLEN) remaining;
            }
            if (buffer == NULL || buffer_length <= 0) {
                return SQL_SUCCESS;
            }
            size_t copy = remaining < (size_t) buffer_length ? remaining : (size_t) buffer_length;
            memcpy(buffer, text + from, copy);
            if (offset != NULL) {
                *offset = copy < remaining ? from + copy : total + 1;
            }
            if (copy < remaining) {
                fl_diag_set(stmt, "01004", "String data, right truncated");
                return SQL_SUCCESS_WITH_INFO;
            }
            return SQL_SUCCESS;
        }
        case SQL_C_SLONG:
        case SQL_C_LONG: {
            long long value;
            int parsed = parse_integer(text, &value);
            if (parsed == -2) {
                return out_of_range(stmt);
            }
            if (parsed != 0) {
                return invalid_number(stmt, text);
            }
            if (value < INT_MIN || value > INT_MAX) {
                return out_of_range(stmt);
            }
            if (buffer != NULL) {
                *(SQLINTEGER *) buffer = (SQLINTEGER) value;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQLINTEGER);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_ULONG: {
            long long value;
            int parsed = parse_integer(text, &value);
            if (parsed == -2) {
                return out_of_range(stmt);
            }
            if (parsed != 0) {
                return invalid_number(stmt, text);
            }
            if (value < 0 || value > (long long) UINT_MAX) {
                return out_of_range(stmt);
            }
            if (buffer != NULL) {
                *(SQLUINTEGER *) buffer = (SQLUINTEGER) value;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQLUINTEGER);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_SSHORT:
        case SQL_C_SHORT: {
            long long value;
            int parsed = parse_integer(text, &value);
            if (parsed == -2) {
                return out_of_range(stmt);
            }
            if (parsed != 0) {
                return invalid_number(stmt, text);
            }
            if (value < SHRT_MIN || value > SHRT_MAX) {
                return out_of_range(stmt);
            }
            if (buffer != NULL) {
                *(SQLSMALLINT *) buffer = (SQLSMALLINT) value;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQLSMALLINT);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_USHORT: {
            long long value;
            int parsed = parse_integer(text, &value);
            if (parsed == -2) {
                return out_of_range(stmt);
            }
            if (parsed != 0) {
                return invalid_number(stmt, text);
            }
            if (value < 0 || value > USHRT_MAX) {
                return out_of_range(stmt);
            }
            if (buffer != NULL) {
                *(SQLUSMALLINT *) buffer = (SQLUSMALLINT) value;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQLUSMALLINT);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_STINYINT:
        case SQL_C_TINYINT:
        case SQL_C_UTINYINT: {
            long long value;
            int parsed = parse_integer(text, &value);
            if (parsed == -2) {
                return out_of_range(stmt);
            }
            if (parsed != 0) {
                return invalid_number(stmt, text);
            }
            if (value < -128 || value > 255) {
                return out_of_range(stmt);
            }
            if (buffer != NULL) {
                *(SQLCHAR *) buffer = (SQLCHAR) value;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQLCHAR);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_SBIGINT:
        case SQL_C_UBIGINT: {
            long long value;
            int parsed = parse_integer(text, &value);
            if (parsed == -2) {
                return out_of_range(stmt);
            }
            if (parsed != 0) {
                return invalid_number(stmt, text);
            }
            if (buffer != NULL) {
                *(SQLBIGINT *) buffer = (SQLBIGINT) value;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQLBIGINT);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_FLOAT: {
            char *end = NULL;
            double value = strtod(text, &end);
            if (end == text) {
                return invalid_number(stmt, text);
            }
            if (buffer != NULL) {
                *(SQLREAL *) buffer = (SQLREAL) value;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQLREAL);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_DOUBLE: {
            char *end = NULL;
            double value = strtod(text, &end);
            if (end == text) {
                return invalid_number(stmt, text);
            }
            if (buffer != NULL) {
                *(SQLDOUBLE *) buffer = value;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQLDOUBLE);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_BIT: {
            int value;
            if (strcmp(text, "1") == 0 || strcasecmp(text, "true") == 0) {
                value = 1;
            } else if (strcmp(text, "0") == 0 || strcasecmp(text, "false") == 0) {
                value = 0;
            } else {
                long long parsed;
                if (parse_integer(text, &parsed) != 0 || (parsed != 0 && parsed != 1)) {
                    return invalid_number(stmt, text);
                }
                value = (int) parsed;
            }
            if (buffer != NULL) {
                *(SQLCHAR *) buffer = (SQLCHAR) value;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQLCHAR);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_TYPE_DATE:
        case SQL_C_DATE: {
            SQL_DATE_STRUCT date;
            if (parse_date_part(text, &date) < 0) {
                return fl_diag_setf(stmt, "22018", "Cannot read '%s' as a DATE", text);
            }
            if (buffer != NULL) {
                *(SQL_DATE_STRUCT *) buffer = date;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQL_DATE_STRUCT);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_TYPE_TIME:
        case SQL_C_TIME: {
            SQL_TIME_STRUCT time;
            if (parse_time_part(text, &time, NULL) < 0) {
                return fl_diag_setf(stmt, "22018", "Cannot read '%s' as a TIME", text);
            }
            if (buffer != NULL) {
                *(SQL_TIME_STRUCT *) buffer = time;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQL_TIME_STRUCT);
            }
            return SQL_SUCCESS;
        }
        case SQL_C_TYPE_TIMESTAMP:
        case SQL_C_TIMESTAMP: {
            SQL_TIMESTAMP_STRUCT ts;
            if (parse_timestamp(text, &ts) < 0) {
                return fl_diag_setf(stmt, "22018", "Cannot read '%s' as a TIMESTAMP", text);
            }
            if (buffer != NULL) {
                *(SQL_TIMESTAMP_STRUCT *) buffer = ts;
            }
            if (str_len_or_ind != NULL) {
                *str_len_or_ind = sizeof(SQL_TIMESTAMP_STRUCT);
            }
            return SQL_SUCCESS;
        }
        default:
            return fl_diag_setf(stmt, "HYC00", "Unsupported target C type %d", (int) c_type);
    }
}

/* ---- binding + fetching ----------------------------------------------------- */

SQLRETURN SQL_API SQLBindCol(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                             SQLSMALLINT TargetType, SQLPOINTER TargetValuePtr,
                             SQLLEN BufferLength, SQLLEN *StrLen_or_IndPtr) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    if (ColumnNumber < 1) {
        return fl_diag_set(stmt, "07009", "Invalid column number");
    }
    if ((int) ColumnNumber > stmt->bound_col_capacity) {
        fl_bound_col *grown = realloc(stmt->bound_cols, ColumnNumber * sizeof(fl_bound_col));
        if (grown == NULL) {
            return fl_diag_set(stmt, "HY001", "Out of memory");
        }
        memset(grown + stmt->bound_col_capacity, 0,
               (size_t) ((int) ColumnNumber - stmt->bound_col_capacity) * sizeof(fl_bound_col));
        stmt->bound_cols = grown;
        stmt->bound_col_capacity = ColumnNumber;
    }
    fl_bound_col *bound = &stmt->bound_cols[ColumnNumber - 1];
    if (TargetValuePtr == NULL) {
        bound->bound = 0;
        return SQL_SUCCESS;
    }
    bound->c_type = TargetType;
    bound->buffer = TargetValuePtr;
    bound->buffer_length = BufferLength;
    bound->str_len_or_ind = StrLen_or_IndPtr;
    bound->bound = 1;
    return SQL_SUCCESS;
}

SQLRETURN SQL_API SQLFetch(SQLHSTMT StatementHandle) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    fl_resultset *set = fl_stmt_current_set(stmt);
    if (set == NULL) {
        return fl_diag_set(stmt, "24000", "No result set");
    }
    /* SQL_ATTR_MAX_ROWS is honoured here, so accepting it is not a promise the
     * driver then breaks by handing back every row anyway. */
    int limit = set->row_count;
    if (stmt->max_rows > 0 && (SQLULEN) limit > stmt->max_rows) {
        limit = (int) stmt->max_rows;
    }
    if (stmt->current_row + 1 >= limit) {
        /* Past the end of the *set*, not of the row limit: parking on the limit
         * left the cursor inside the fetched range, and SQLGetData — whose
         * guard is against set->row_count — then handed back the next row the
         * caller had deliberately not been given. */
        stmt->current_row = set->row_count;
        if (stmt->rows_fetched_ptr != NULL) {
            *stmt->rows_fetched_ptr = 0;
        }
        if (stmt->row_status_ptr != NULL) {
            stmt->row_status_ptr[0] = SQL_ROW_NOROW;
        }
        return SQL_NO_DATA;
    }
    stmt->current_row++;
    stmt->getdata_col = -1;
    stmt->getdata_offset = 0;
    if (stmt->rows_fetched_ptr != NULL) {
        *stmt->rows_fetched_ptr = 1;
    }
    if (stmt->row_status_ptr != NULL) {
        stmt->row_status_ptr[0] = SQL_ROW_SUCCESS;
    }

    /* deliver bound columns */
    SQLRETURN rc = SQL_SUCCESS;
    for (int i = 0; i < stmt->bound_col_capacity && i < set->column_count; i++) {
        fl_bound_col *bound = &stmt->bound_cols[i];
        if (!bound->bound) {
            continue;
        }
        const fl_cell *cell = &set->cells[stmt->current_row * set->column_count + i];
        SQLRETURN one = convert_cell(stmt, &set->columns[i], cell, bound->c_type,
                                     bound->buffer, bound->buffer_length,
                                     bound->str_len_or_ind, NULL);
        if (one == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) {
            rc = SQL_SUCCESS_WITH_INFO;
        } else if (one != SQL_SUCCESS && one != SQL_SUCCESS_WITH_INFO) {
            return one;
        }
    }
    return rc;
}

SQLRETURN SQL_API SQLFetchScroll(SQLHSTMT StatementHandle, SQLSMALLINT FetchOrientation,
                                 SQLLEN FetchOffset) {
    (void) FetchOffset;
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    if (FetchOrientation != SQL_FETCH_NEXT) {
        fl_diag_clear(stmt);
        return fl_diag_set(stmt, "HY106", "Only SQL_FETCH_NEXT is supported");
    }
    return SQLFetch(StatementHandle);
}

SQLRETURN SQL_API SQLGetData(SQLHSTMT StatementHandle, SQLUSMALLINT ColumnNumber,
                             SQLSMALLINT TargetType, SQLPOINTER TargetValuePtr,
                             SQLLEN BufferLength, SQLLEN *StrLen_or_IndPtr) {
    fl_stmt *stmt = (fl_stmt *) StatementHandle;
    if (stmt == NULL) {
        return SQL_INVALID_HANDLE;
    }
    fl_diag_clear(stmt);
    fl_resultset *set = fl_stmt_current_set(stmt);
    if (set == NULL || stmt->current_row < 0 || stmt->current_row >= set->row_count) {
        return fl_diag_set(stmt, "24000", "Not positioned on a row");
    }
    fl_column *column = column_at(stmt, ColumnNumber);
    if (column == NULL) {
        return fl_diag_set(stmt, "07009", "Invalid column number");
    }
    /* chunked retrieval bookkeeping: a new column starts a fresh read */
    if (stmt->getdata_col != (int) ColumnNumber) {
        stmt->getdata_col = (int) ColumnNumber;
        stmt->getdata_offset = 0;
    }
    const fl_cell *cell = &set->cells[stmt->current_row * set->column_count + (ColumnNumber - 1)];
    return convert_cell(stmt, column, cell, TargetType, TargetValuePtr,
                        BufferLength, StrLen_or_IndPtr, &stmt->getdata_offset);
}
