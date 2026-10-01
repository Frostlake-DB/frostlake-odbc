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
 * Minimal HTTP/1.1 client over POSIX sockets — enough for the engine's JSON
 * endpoints, which speak plain HTTP with explicit Content-Length (chunked
 * transfer is handled anyway for robustness). No TLS: the Frostlake HTTP
 * server is plain HTTP by design, same as the JDBC transport.
 */
#ifndef FL_HTTP_H
#define FL_HTTP_H

#include <stddef.h>

/*
 * POST `body` to http://host:port/path. On success returns 0 and hands back the
 * HTTP status plus the response body (caller frees). On transport failure
 * returns -1 and fills *error with a malloc'd description (caller frees).
 * timeout_seconds applies to connect and to each read/write (0 = no timeout).
 */
int fl_http_post(const char *host, int port, const char *path,
                 const char *body, int timeout_seconds,
                 int *status_out, char **body_out, char **error_out);

/* GET http://host:port/path, same contract. */
int fl_http_get(const char *host, int port, const char *path,
                int timeout_seconds, int *status_out, char **body_out, char **error_out);

/* DELETE http://host:port/path, same contract. */
int fl_http_delete(const char *host, int port, const char *path,
                   int timeout_seconds, int *status_out, char **body_out, char **error_out);

#endif /* FL_HTTP_H */
