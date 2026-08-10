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
 * Minimal JSON reader/writer for the Frostlake wire protocol.
 *
 * Hand-rolled instead of vendored for one load-bearing reason: numbers keep
 * their ORIGINAL LEXEME. The engine sends NUMBER cells as JSON numbers with up
 * to 38 digits; a parser that funnels them through a C double would corrupt
 * exactly the values the engine goes out of its way to keep exact.
 */
#ifndef FL_JSON_H
#define FL_JSON_H

#include <stddef.h>

typedef enum fl_json_kind {
    FL_JSON_NULL,
    FL_JSON_BOOL,
    FL_JSON_NUMBER,
    FL_JSON_STRING,
    FL_JSON_ARRAY,
    FL_JSON_OBJECT
} fl_json_kind;

typedef struct fl_json {
    fl_json_kind kind;
    /* STRING: decoded UTF-8 text. NUMBER: the raw lexeme. BOOL: "true"/"false". */
    char *text;
    /* OBJECT members / ARRAY elements */
    struct fl_json **children;
    char **keys; /* OBJECT only, parallel to children */
    size_t child_count;
} fl_json;

/* Parse a complete JSON document; returns NULL on malformed input. */
fl_json *fl_json_parse(const char *input, size_t length);
void fl_json_free(fl_json *node);

/* Lookups (NULL when absent or of a different kind). */
fl_json *fl_json_get(const fl_json *object, const char *key);
const char *fl_json_get_string(const fl_json *object, const char *key);
fl_json *fl_json_at(const fl_json *array, size_t index);
int fl_json_get_bool(const fl_json *object, const char *key, int fallback);
long fl_json_get_long(const fl_json *object, const char *key, long fallback);

/* Append a JSON string literal (with quotes, escaped) to a growable buffer. */
typedef struct fl_strbuf {
    char *data;
    size_t length;
    size_t capacity;
} fl_strbuf;

void fl_strbuf_init(fl_strbuf *buf);
void fl_strbuf_free(fl_strbuf *buf);
int fl_strbuf_append(fl_strbuf *buf, const char *text);
int fl_strbuf_append_len(fl_strbuf *buf, const char *text, size_t length);
int fl_strbuf_append_json_string(fl_strbuf *buf, const char *text);

#endif /* FL_JSON_H */
