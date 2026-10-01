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

#include "http.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static char *http_error(const char *what, const char *detail) {
    size_t length = strlen(what) + (detail != NULL ? strlen(detail) : 0) + 4;
    char *message = malloc(length);
    if (message != NULL) {
        snprintf(message, length, "%s%s%s", what, detail != NULL ? ": " : "", detail != NULL ? detail : "");
    }
    return message;
}

static int connect_to(const char *host, int port, int timeout_seconds, char **error_out) {
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *addresses = NULL;
    int rc = getaddrinfo(host, port_text, &hints, &addresses);
    if (rc != 0) {
        *error_out = http_error("cannot resolve host", gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (timeout_seconds > 0) {
            struct timeval tv;
            tv.tv_sec = timeout_seconds;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        *error_out = http_error("cannot connect", strerror(errno));
    }
    return fd;
}

static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t) n;
    }
    return 0;
}

/* Read until EOF into a growable buffer; the response is then parsed in place. */
static char *read_all(int fd, size_t *length_out) {
    size_t capacity = 8192;
    size_t length = 0;
    char *data = malloc(capacity);
    if (data == NULL) {
        return NULL;
    }
    for (;;) {
        if (length + 4096 + 1 > capacity) {
            capacity *= 2;
            char *grown = realloc(data, capacity);
            if (grown == NULL) {
                free(data);
                return NULL;
            }
            data = grown;
        }
        ssize_t n = recv(fd, data + length, 4096, 0);
        if (n < 0) {
            free(data);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        length += (size_t) n;
    }
    data[length] = '\0';
    *length_out = length;
    return data;
}

/* Find the header value (case-insensitive name) inside the raw header block. */
static const char *find_header(const char *headers, const char *name, size_t *value_length) {
    size_t name_length = strlen(name);
    const char *line = headers;
    while (line != NULL && *line != '\0') {
        const char *next = strstr(line, "\r\n");
        size_t line_length = next != NULL ? (size_t) (next - line) : strlen(line);
        if (line_length > name_length + 1 && strncasecmp(line, name, name_length) == 0
            && line[name_length] == ':') {
            const char *value = line + name_length + 1;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            *value_length = (size_t) (line + line_length - value);
            return value;
        }
        line = next != NULL ? next + 2 : NULL;
    }
    return NULL;
}

/* Decode a chunked body in place; returns the decoded length or -1. */
static long decode_chunked(char *body, size_t body_length) {
    size_t read_pos = 0;
    size_t write_pos = 0;
    while (read_pos < body_length) {
        char *line_end = memmem(body + read_pos, body_length - read_pos, "\r\n", 2);
        if (line_end == NULL) {
            return -1;
        }
        long chunk = strtol(body + read_pos, NULL, 16);
        read_pos = (size_t) (line_end - body) + 2;
        if (chunk == 0) {
            break;
        }
        if (read_pos + (size_t) chunk > body_length) {
            return -1;
        }
        memmove(body + write_pos, body + read_pos, (size_t) chunk);
        write_pos += (size_t) chunk;
        read_pos += (size_t) chunk;
        if (read_pos + 2 <= body_length && body[read_pos] == '\r' && body[read_pos + 1] == '\n') {
            read_pos += 2;
        }
    }
    body[write_pos] = '\0';
    return (long) write_pos;
}

static int http_request(const char *host, int port, const char *method, const char *path,
                        const char *body, int timeout_seconds,
                        int *status_out, char **body_out, char **error_out) {
    *body_out = NULL;
    *error_out = NULL;

    int fd = connect_to(host, port, timeout_seconds, error_out);
    if (fd < 0) {
        return -1;
    }

    size_t body_length = body != NULL ? strlen(body) : 0;
    size_t head_capacity = strlen(method) + strlen(path) + strlen(host) + 256;
    char *head = malloc(head_capacity);
    if (head == NULL) {
        close(fd);
        *error_out = http_error("out of memory", NULL);
        return -1;
    }
    snprintf(head, head_capacity,
             "%s %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             method, path, host, port, body_length);

    int failed = send_all(fd, head, strlen(head)) != 0
        || (body_length > 0 && send_all(fd, body, body_length) != 0);
    free(head);
    if (failed) {
        close(fd);
        *error_out = http_error("cannot send request", strerror(errno));
        return -1;
    }

    size_t raw_length = 0;
    char *raw = read_all(fd, &raw_length);
    close(fd);
    if (raw == NULL) {
        *error_out = http_error("cannot read response", strerror(errno));
        return -1;
    }

    /* status line */
    if (strncmp(raw, "HTTP/1.", 7) != 0 || raw_length < 12) {
        free(raw);
        *error_out = http_error("malformed HTTP response", NULL);
        return -1;
    }
    *status_out = atoi(raw + 9);

    char *header_end = memmem(raw, raw_length, "\r\n\r\n", 4);
    if (header_end == NULL) {
        free(raw);
        *error_out = http_error("malformed HTTP response", "no header terminator");
        return -1;
    }
    *header_end = '\0';
    char *payload = header_end + 4;
    size_t payload_length = raw_length - (size_t) (payload - raw);

    size_t value_length = 0;
    const char *transfer_encoding = find_header(raw, "Transfer-Encoding", &value_length);
    if (transfer_encoding != NULL && strncasecmp(transfer_encoding, "chunked", 7) == 0) {
        long decoded = decode_chunked(payload, payload_length);
        if (decoded < 0) {
            free(raw);
            *error_out = http_error("malformed chunked response", NULL);
            return -1;
        }
        payload_length = (size_t) decoded;
    } else {
        const char *content_length = find_header(raw, "Content-Length", &value_length);
        if (content_length != NULL) {
            long declared = strtol(content_length, NULL, 10);
            if (declared >= 0 && (size_t) declared <= payload_length) {
                payload_length = (size_t) declared;
            }
        }
    }

    char *result = malloc(payload_length + 1);
    if (result == NULL) {
        free(raw);
        *error_out = http_error("out of memory", NULL);
        return -1;
    }
    memcpy(result, payload, payload_length);
    result[payload_length] = '\0';
    free(raw);
    *body_out = result;
    return 0;
}

int fl_http_post(const char *host, int port, const char *path,
                 const char *body, int timeout_seconds,
                 int *status_out, char **body_out, char **error_out) {
    return http_request(host, port, "POST", path, body, timeout_seconds,
                        status_out, body_out, error_out);
}

int fl_http_get(const char *host, int port, const char *path,
                int timeout_seconds, int *status_out, char **body_out, char **error_out) {
    return http_request(host, port, "GET", path, NULL, timeout_seconds,
                        status_out, body_out, error_out);
}

int fl_http_delete(const char *host, int port, const char *path,
                   int timeout_seconds, int *status_out, char **body_out, char **error_out) {
    return http_request(host, port, "DELETE", path, NULL, timeout_seconds,
                        status_out, body_out, error_out);
}
