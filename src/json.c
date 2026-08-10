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

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Objects and arrays are parsed by recursion, so the nesting a response may
 * carry has to be bounded: a reply of 200000 open brackets otherwise walks the
 * C stack straight off the end and the process dies with SIGSEGV. Real replies
 * nest four or five deep — resultSets, a set, its rows, one row, a cell. */
#define FL_JSON_MAX_DEPTH 64

typedef struct parser {
    const char *cur;
    const char *end;
    int depth;
} parser;

static fl_json *parse_value(parser *p);

/* ---- growable buffer ------------------------------------------------------ */

void fl_strbuf_init(fl_strbuf *buf) {
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

void fl_strbuf_free(fl_strbuf *buf) {
    free(buf->data);
    fl_strbuf_init(buf);
}

static int strbuf_reserve(fl_strbuf *buf, size_t extra) {
    if (buf->length + extra + 1 <= buf->capacity) {
        return 0;
    }
    size_t wanted = buf->capacity ? buf->capacity : 256;
    while (wanted < buf->length + extra + 1) {
        wanted *= 2;
    }
    char *grown = realloc(buf->data, wanted);
    if (grown == NULL) {
        return -1;
    }
    buf->data = grown;
    buf->capacity = wanted;
    return 0;
}

int fl_strbuf_append_len(fl_strbuf *buf, const char *text, size_t length) {
    if (strbuf_reserve(buf, length) != 0) {
        return -1;
    }
    memcpy(buf->data + buf->length, text, length);
    buf->length += length;
    buf->data[buf->length] = '\0';
    return 0;
}

int fl_strbuf_append(fl_strbuf *buf, const char *text) {
    return fl_strbuf_append_len(buf, text, strlen(text));
}

int fl_strbuf_append_json_string(fl_strbuf *buf, const char *text) {
    if (fl_strbuf_append_len(buf, "\"", 1) != 0) {
        return -1;
    }
    for (const unsigned char *c = (const unsigned char *) text; *c; c++) {
        char escaped[8];
        switch (*c) {
            case '"': strcpy(escaped, "\\\""); break;
            case '\\': strcpy(escaped, "\\\\"); break;
            case '\b': strcpy(escaped, "\\b"); break;
            case '\f': strcpy(escaped, "\\f"); break;
            case '\n': strcpy(escaped, "\\n"); break;
            case '\r': strcpy(escaped, "\\r"); break;
            case '\t': strcpy(escaped, "\\t"); break;
            default:
                if (*c < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", *c);
                } else {
                    escaped[0] = (char) *c;
                    escaped[1] = '\0';
                }
        }
        if (fl_strbuf_append(buf, escaped) != 0) {
            return -1;
        }
    }
    return fl_strbuf_append_len(buf, "\"", 1);
}

/* ---- parsing -------------------------------------------------------------- */

static void skip_ws(parser *p) {
    while (p->cur < p->end
           && (*p->cur == ' ' || *p->cur == '\t' || *p->cur == '\n' || *p->cur == '\r')) {
        p->cur++;
    }
}

static fl_json *node_new(fl_json_kind kind) {
    fl_json *node = calloc(1, sizeof(fl_json));
    if (node != NULL) {
        node->kind = kind;
    }
    return node;
}

void fl_json_free(fl_json *node) {
    if (node == NULL) {
        return;
    }
    for (size_t i = 0; i < node->child_count; i++) {
        fl_json_free(node->children[i]);
        if (node->keys != NULL) {
            free(node->keys[i]);
        }
    }
    free(node->children);
    free(node->keys);
    free(node->text);
    free(node);
}

static int node_add_child(fl_json *parent, char *key, fl_json *child) {
    size_t wanted = parent->child_count + 1;
    fl_json **children = realloc(parent->children, wanted * sizeof(fl_json *));
    if (children == NULL) {
        return -1;
    }
    parent->children = children;
    if (parent->kind == FL_JSON_OBJECT) {
        char **keys = realloc(parent->keys, wanted * sizeof(char *));
        if (keys == NULL) {
            return -1;
        }
        parent->keys = keys;
        parent->keys[parent->child_count] = key;
    }
    parent->children[parent->child_count] = child;
    parent->child_count = wanted;
    return 0;
}

/* Encode one code point as UTF-8 into the buffer. */
static int append_utf8(fl_strbuf *buf, unsigned long cp) {
    char bytes[4];
    size_t n;
    if (cp < 0x80) {
        bytes[0] = (char) cp;
        n = 1;
    } else if (cp < 0x800) {
        bytes[0] = (char) (0xC0 | (cp >> 6));
        bytes[1] = (char) (0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (char) (0xE0 | (cp >> 12));
        bytes[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = (char) (0x80 | (cp & 0x3F));
        n = 3;
    } else {
        bytes[0] = (char) (0xF0 | (cp >> 18));
        bytes[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
        bytes[3] = (char) (0x80 | (cp & 0x3F));
        n = 4;
    }
    return fl_strbuf_append_len(buf, bytes, n);
}

static int parse_hex4(parser *p, unsigned long *out) {
    if (p->end - p->cur < 4) {
        return -1;
    }
    unsigned long value = 0;
    for (int i = 0; i < 4; i++) {
        char c = p->cur[i];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= (unsigned long) (c - '0');
        else if (c >= 'a' && c <= 'f') value |= (unsigned long) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= (unsigned long) (c - 'A' + 10);
        else return -1;
    }
    p->cur += 4;
    *out = value;
    return 0;
}

/* Parses the body of a string literal after the opening quote; returns the
 * decoded text (caller frees) or NULL. Surrogate pairs are recombined. */
static char *parse_string_body(parser *p) {
    fl_strbuf buf;
    fl_strbuf_init(&buf);
    while (p->cur < p->end && *p->cur != '"') {
        unsigned char c = (unsigned char) *p->cur;
        if (c == '\\') {
            p->cur++;
            if (p->cur >= p->end) {
                goto fail;
            }
            char esc = *p->cur++;
            switch (esc) {
                case '"': if (fl_strbuf_append_len(&buf, "\"", 1)) goto fail; break;
                case '\\': if (fl_strbuf_append_len(&buf, "\\", 1)) goto fail; break;
                case '/': if (fl_strbuf_append_len(&buf, "/", 1)) goto fail; break;
                case 'b': if (fl_strbuf_append_len(&buf, "\b", 1)) goto fail; break;
                case 'f': if (fl_strbuf_append_len(&buf, "\f", 1)) goto fail; break;
                case 'n': if (fl_strbuf_append_len(&buf, "\n", 1)) goto fail; break;
                case 'r': if (fl_strbuf_append_len(&buf, "\r", 1)) goto fail; break;
                case 't': if (fl_strbuf_append_len(&buf, "\t", 1)) goto fail; break;
                case 'u': {
                    unsigned long cp;
                    if (parse_hex4(p, &cp) != 0) {
                        goto fail;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && p->end - p->cur >= 6
                        && p->cur[0] == '\\' && p->cur[1] == 'u') {
                        p->cur += 2;
                        unsigned long low;
                        if (parse_hex4(p, &low) != 0) {
                            goto fail;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    if (append_utf8(&buf, cp) != 0) {
                        goto fail;
                    }
                    break;
                }
                default:
                    goto fail;
            }
        } else {
            if (fl_strbuf_append_len(&buf, (const char *) p->cur, 1) != 0) {
                goto fail;
            }
            p->cur++;
        }
    }
    if (p->cur >= p->end) {
        goto fail;
    }
    p->cur++; /* closing quote */
    if (buf.data == NULL) {
        /* empty string: still hand back an owned buffer */
        if (fl_strbuf_append_len(&buf, "", 0) != 0) {
            goto fail;
        }
    }
    return buf.data;
fail:
    fl_strbuf_free(&buf);
    return NULL;
}

static fl_json *parse_string(parser *p) {
    p->cur++; /* opening quote */
    char *text = parse_string_body(p);
    if (text == NULL) {
        return NULL;
    }
    fl_json *node = node_new(FL_JSON_STRING);
    if (node == NULL) {
        free(text);
        return NULL;
    }
    node->text = text;
    return node;
}

static fl_json *parse_number(parser *p) {
    const char *start = p->cur;
    if (p->cur < p->end && *p->cur == '-') {
        p->cur++;
    }
    while (p->cur < p->end
           && ((*p->cur >= '0' && *p->cur <= '9') || *p->cur == '.'
               || *p->cur == 'e' || *p->cur == 'E' || *p->cur == '+' || *p->cur == '-')) {
        p->cur++;
    }
    if (p->cur == start) {
        return NULL;
    }
    fl_json *node = node_new(FL_JSON_NUMBER);
    if (node == NULL) {
        return NULL;
    }
    size_t length = (size_t) (p->cur - start);
    node->text = malloc(length + 1);
    if (node->text == NULL) {
        free(node);
        return NULL;
    }
    memcpy(node->text, start, length);
    node->text[length] = '\0';
    return node;
}

static char *dup_text(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

static fl_json *parse_literal(parser *p, const char *word, fl_json_kind kind, const char *text) {
    size_t length = strlen(word);
    if ((size_t) (p->end - p->cur) < length || strncmp(p->cur, word, length) != 0) {
        return NULL;
    }
    p->cur += length;
    fl_json *node = node_new(kind);
    if (node == NULL) {
        return NULL;
    }
    if (text != NULL) {
        node->text = dup_text(text);
        if (node->text == NULL) {
            free(node);
            return NULL;
        }
    }
    return node;
}

static fl_json *parse_array(parser *p) {
    p->cur++; /* '[' */
    fl_json *node = node_new(FL_JSON_ARRAY);
    if (node == NULL) {
        return NULL;
    }
    skip_ws(p);
    if (p->cur < p->end && *p->cur == ']') {
        p->cur++;
        return node;
    }
    for (;;) {
        fl_json *child = parse_value(p);
        if (child == NULL || node_add_child(node, NULL, child) != 0) {
            fl_json_free(child);
            fl_json_free(node);
            return NULL;
        }
        skip_ws(p);
        if (p->cur < p->end && *p->cur == ',') {
            p->cur++;
            continue;
        }
        if (p->cur < p->end && *p->cur == ']') {
            p->cur++;
            return node;
        }
        fl_json_free(node);
        return NULL;
    }
}

static fl_json *parse_object(parser *p) {
    p->cur++; /* '{' */
    fl_json *node = node_new(FL_JSON_OBJECT);
    if (node == NULL) {
        return NULL;
    }
    skip_ws(p);
    if (p->cur < p->end && *p->cur == '}') {
        p->cur++;
        return node;
    }
    for (;;) {
        skip_ws(p);
        if (p->cur >= p->end || *p->cur != '"') {
            fl_json_free(node);
            return NULL;
        }
        p->cur++;
        char *key = parse_string_body(p);
        if (key == NULL) {
            fl_json_free(node);
            return NULL;
        }
        skip_ws(p);
        if (p->cur >= p->end || *p->cur != ':') {
            free(key);
            fl_json_free(node);
            return NULL;
        }
        p->cur++;
        fl_json *child = parse_value(p);
        if (child == NULL || node_add_child(node, key, child) != 0) {
            free(key);
            fl_json_free(child);
            fl_json_free(node);
            return NULL;
        }
        skip_ws(p);
        if (p->cur < p->end && *p->cur == ',') {
            p->cur++;
            continue;
        }
        if (p->cur < p->end && *p->cur == '}') {
            p->cur++;
            return node;
        }
        fl_json_free(node);
        return NULL;
    }
}

static fl_json *parse_value(parser *p) {
    skip_ws(p);
    if (p->cur >= p->end) {
        return NULL;
    }
    switch (*p->cur) {
        case '{':
        case '[': {
            if (p->depth >= FL_JSON_MAX_DEPTH) {
                return NULL;
            }
            p->depth++;
            fl_json *nested = *p->cur == '{' ? parse_object(p) : parse_array(p);
            p->depth--;
            return nested;
        }
        case '"': return parse_string(p);
        case 't': return parse_literal(p, "true", FL_JSON_BOOL, "true");
        case 'f': return parse_literal(p, "false", FL_JSON_BOOL, "false");
        case 'n': return parse_literal(p, "null", FL_JSON_NULL, NULL);
        default: return parse_number(p);
    }
}

fl_json *fl_json_parse(const char *input, size_t length) {
    parser p;
    p.cur = input;
    p.end = input + length;
    p.depth = 0;
    fl_json *root = parse_value(&p);
    if (root == NULL) {
        return NULL;
    }
    skip_ws(&p);
    if (p.cur != p.end) {
        fl_json_free(root);
        return NULL;
    }
    return root;
}

/* ---- lookups -------------------------------------------------------------- */

fl_json *fl_json_get(const fl_json *object, const char *key) {
    if (object == NULL || object->kind != FL_JSON_OBJECT) {
        return NULL;
    }
    for (size_t i = 0; i < object->child_count; i++) {
        if (strcmp(object->keys[i], key) == 0) {
            return object->children[i];
        }
    }
    return NULL;
}

const char *fl_json_get_string(const fl_json *object, const char *key) {
    fl_json *node = fl_json_get(object, key);
    return node != NULL && node->kind == FL_JSON_STRING ? node->text : NULL;
}

fl_json *fl_json_at(const fl_json *array, size_t index) {
    if (array == NULL || array->kind != FL_JSON_ARRAY || index >= array->child_count) {
        return NULL;
    }
    return array->children[index];
}

int fl_json_get_bool(const fl_json *object, const char *key, int fallback) {
    fl_json *node = fl_json_get(object, key);
    if (node == NULL || node->kind != FL_JSON_BOOL) {
        return fallback;
    }
    return strcmp(node->text, "true") == 0;
}

long fl_json_get_long(const fl_json *object, const char *key, long fallback) {
    fl_json *node = fl_json_get(object, key);
    if (node == NULL || node->kind != FL_JSON_NUMBER) {
        return fallback;
    }
    return strtol(node->text, NULL, 10);
}
