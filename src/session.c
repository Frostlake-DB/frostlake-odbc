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
 * What a statement leaves on the engine session. After an application statement succeeds, the
 * connection notes whether the session now holds something a fresh session would not have — a
 * moved scope, a session variable or setting, a temporary object — and whether a transaction is
 * open. proto.c reads both when the engine says it no longer holds the session: with either, the
 * statement is reported rather than sent again on a fresh one.
 *
 * The SQL is read with the same scanner parameter binding uses (fl_sql_skip), so the two cannot
 * disagree about what sits inside a literal, a quoted identifier, a comment or a $$ body. A
 * scripting block is split along with everything else, which only makes the checks more willing
 * to flag a request: the safe direction to be wrong in.
 */

#include "fl_odbc.h"

#include <string.h>

#define WORD_LIMIT 16
#define WORD_LENGTH 32

/* The words that may sit between CREATE/DROP/ALTER and the kind of object being named. */
static const char *const MODIFIERS[] = {
    "OR", "REPLACE", "TRANSIENT", "TEMPORARY", "TEMP", "VOLATILE", "LOCAL", "GLOBAL", "SECURE",
    "IF", "NOT", "EXISTS", "PUBLIC", "PRIVATE", "ICEBERG", "DYNAMIC", "HYBRID", "EVENT",
    "RECURSIVE", "MATERIALIZED", "EXTERNAL", NULL
};

/* The modifiers that make a created object live and die with the session. */
static const char *const TEMPORARY[] = { "TEMPORARY", "TEMP", "VOLATILE", NULL };

static int listed(const char *const *list, const char *word) {
    for (int i = 0; list[i] != NULL; i++) {
        if (strcmp(list[i], word) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_word_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == '_' || c == '$';
}

/* Up to WORD_LIMIT leading words of the statement [start, end), upper-cased, skipping whitespace
 * and comments and stopping at the first thing that is not a word. A leading literal or quoted
 * identifier means the statement does not start with a keyword at all. */
static int leading_words(const char *start, const char *end, char words[][WORD_LENGTH]) {
    int count = 0;
    const char *c = start;
    while (c < end && count < WORD_LIMIT) {
        if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' || *c == '\f' || *c == '\v') {
            c++;
            continue;
        }
        if ((c[0] == '-' && c[1] == '-') || (c[0] == '/' && (c[1] == '/' || c[1] == '*'))) {
            const char *past = fl_sql_skip(c);
            c = past != NULL ? past : end;
            continue;
        }
        if (!is_word_char(*c)) {
            break;
        }
        size_t length = 0;
        while (c < end && is_word_char(*c)) {
            if (length + 1 < WORD_LENGTH) {
                words[count][length++] = (char) (*c >= 'a' && *c <= 'z' ? *c - 'a' + 'A' : *c);
            }
            c++;
        }
        words[count][length] = '\0';
        count++;
    }
    return count;
}

/* The first word after the verb that is not a modifier, or "" when there is none. */
static const char *object_kind(char words[][WORD_LENGTH], int count) {
    for (int i = 1; i < count; i++) {
        if (!listed(MODIFIERS, words[i])) {
            return words[i];
        }
    }
    return "";
}

/* Whether one statement leaves behind state a fresh session would not have: a moved scope (USE,
 * CREATE or DROP of a DATABASE or SCHEMA), a session variable or setting (SET, UNSET, ALTER
 * SESSION), or a temporary object. CREATE TABLE and its kind leave the session as it was. */
static int touches_session(char words[][WORD_LENGTH], int count) {
    if (count == 0) {
        return 0;
    }
    const char *verb = words[0];
    if (strcmp(verb, "USE") == 0 || strcmp(verb, "SET") == 0 || strcmp(verb, "UNSET") == 0) {
        return 1;
    }
    if (strcmp(verb, "ALTER") == 0) {
        return strcmp(object_kind(words, count), "SESSION") == 0;
    }
    if (strcmp(verb, "CREATE") == 0 || strcmp(verb, "DROP") == 0) {
        const char *kind = object_kind(words, count);
        if (strcmp(kind, "DATABASE") == 0 || strcmp(kind, "SCHEMA") == 0) {
            return 1;
        }
        if (strcmp(verb, "CREATE") == 0) {
            for (int i = 1; i < count && listed(MODIFIERS, words[i]); i++) {
                if (listed(TEMPORARY, words[i])) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* What one statement does to the session's transaction: 1 opens one, -1 ends one, 0 neither.
 * BEGIN on its own — or followed by TRANSACTION, WORK or NAME — opens one, and so does START
 * TRANSACTION; BEGIN followed by a statement opens a scripting block instead. */
static int transaction_effect(char words[][WORD_LENGTH], int count) {
    if (count >= 1 && strcmp(words[0], "BEGIN") == 0) {
        if (count == 1 || strcmp(words[1], "TRANSACTION") == 0 || strcmp(words[1], "WORK") == 0
            || strcmp(words[1], "NAME") == 0) {
            return 1;
        }
        return 0;
    }
    if (count >= 2 && strcmp(words[0], "START") == 0 && strcmp(words[1], "TRANSACTION") == 0) {
        return 1;
    }
    if (count >= 1 && (strcmp(words[0], "COMMIT") == 0 || strcmp(words[0], "ROLLBACK") == 0)) {
        return -1;
    }
    return 0;
}

/* One statement of a request that succeeded. With SQL_ATTR_AUTOCOMMIT off, every statement but
 * the ones that end a transaction leaves one open: the engine opens it implicitly. */
static void note_statement(fl_dbc *dbc, const char *start, const char *end) {
    char words[WORD_LIMIT][WORD_LENGTH];
    int count = leading_words(start, end, words);
    if (count == 0) {
        return;
    }
    if (touches_session(words, count)) {
        dbc->session_dirty = 1;
    }
    int effect = transaction_effect(words, count);
    if (effect > 0) {
        dbc->in_transaction = 1;
    } else if (effect < 0) {
        dbc->in_transaction = 0;
    } else if (!dbc->autocommit) {
        dbc->in_transaction = 1;
    }
}

void fl_session_note(fl_dbc *dbc, const char *sql) {
    const char *start = sql;
    const char *c = sql;
    while (*c != '\0') {
        const char *past = fl_sql_skip(c);
        if (past != NULL) {
            c = past;
            continue;
        }
        if (*c == ';') {
            note_statement(dbc, start, c);
            start = c + 1;
        }
        c++;
    }
    note_statement(dbc, start, c);
}
