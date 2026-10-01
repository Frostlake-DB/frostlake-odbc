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
 * How a connection keeps its idea of the engine session in step with the engine's: the
 * requireSession flag, recovery from a session the engine no longer holds, and the release on
 * SQLDisconnect.
 *
 * The scripted checks run the driver — loaded with dlopen and called directly, as direct.c does —
 * against an HTTP server in this process that answers from a script, so every request a scenario
 * makes is one it scripted. With FROSTLAKE_TEST_PORT naming a live server, the last checks put
 * that same server between the driver and the engine as a relay: it is how they learn the session
 * id to release behind the connection's back.
 *
 *   build/session <path-to-libfrostlakeodbc.so>
 */

#include <sql.h>
#include <sqlext.h>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ---- checks ---------------------------------------------------------------- */

static int checks;
static int failures;

static void check(int condition, const char *what) {
    checks++;
    if (condition) {
        printf("ok %d - %s\n", checks, what);
        return;
    }
    failures++;
    printf("not ok %d - %s\n", checks, what);
    fprintf(stderr, "FAILED at check %d: %s\n", checks, what);
}

/* ---- the scripted server --------------------------------------------------- */

enum { REPLY, CLOSE, HANG };

#define SCRIPT_LIMIT 32
#define SEEN_LIMIT 64
#define BODY_LIMIT 4096

typedef struct scripted {
    int action;
    int status;
    int hang_ms;
    char body[1024];
} scripted;

typedef struct seen {
    char method[16];
    char path[256];
    char body[BODY_LIMIT];
} seen;

static struct {
    int fd;
    int port;
    pthread_t thread;
    scripted script[SCRIPT_LIMIT];
    int script_count;
    int next;
    seen log[SEEN_LIMIT];
    int seen_count;
    /* When set, every request is passed to the engine on this port and its answer passed back. */
    int relay_port;
    /* The last session id a relayed answer named. */
    char relayed_session[128];
    volatile int stop;
} server;

static void write_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t n = send(fd, data, length, MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        data += n;
        length -= (size_t) n;
    }
}

static void set_timeout(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* Reads one whole request — head, then the Content-Length body — into `raw`; answers its length,
 * or -1. */
static int read_request(int fd, char *raw, size_t capacity) {
    size_t length = 0;
    char *head_end = NULL;
    while (head_end == NULL) {
        if (length + 1 >= capacity) {
            return -1;
        }
        ssize_t n = recv(fd, raw + length, capacity - length - 1, 0);
        if (n <= 0) {
            return -1;
        }
        length += (size_t) n;
        raw[length] = '\0';
        head_end = strstr(raw, "\r\n\r\n");
    }
    size_t body_length = 0;
    const char *field = strcasestr(raw, "\r\nContent-Length:");
    if (field != NULL && field < head_end) {
        body_length = (size_t) strtoul(field + 17, NULL, 10);
    }
    size_t total = (size_t) (head_end - raw) + 4 + body_length;
    while (length < total) {
        if (length + 1 >= capacity) {
            return -1;
        }
        ssize_t n = recv(fd, raw + length, capacity - length - 1, 0);
        if (n <= 0) {
            return -1;
        }
        length += (size_t) n;
        raw[length] = '\0';
    }
    return (int) length;
}

static void send_reply(int fd, int status, const char *body) {
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
             "Connection: close\r\n\r\n",
             status, status == 200 ? "OK" : "Error", strlen(body));
    write_all(fd, head, strlen(head));
    write_all(fd, body, strlen(body));
}

/* Passes the request to the engine and its whole answer back, noting the session it names. */
static void relay(int client, const char *raw, int length) {
    int engine = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short) server.relay_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    set_timeout(engine, 60);
    if (connect(engine, (struct sockaddr *) &address, sizeof(address)) != 0) {
        close(engine);
        return;
    }
    write_all(engine, raw, (size_t) length);
    size_t capacity = 1 << 16;
    size_t got = 0;
    char *answer = malloc(capacity);
    for (;;) {
        if (got + 4096 + 1 > capacity) {
            capacity *= 2;
            answer = realloc(answer, capacity);
        }
        ssize_t n = recv(engine, answer + got, 4096, 0);
        if (n <= 0) {
            break;
        }
        got += (size_t) n;
    }
    close(engine);
    answer[got] = '\0';
    const char *named = strstr(answer, "\"sessionId\":\"");
    if (named != NULL) {
        named += 13;
        size_t n = 0;
        while (named[n] != '\0' && named[n] != '"' && n + 1 < sizeof(server.relayed_session)) {
            n++;
        }
        memcpy(server.relayed_session, named, n);
        server.relayed_session[n] = '\0';
    }
    write_all(client, answer, got);
    free(answer);
}

static void handle(int client) {
    static char raw[1 << 16];
    set_timeout(client, 10);
    int length = read_request(client, raw, sizeof(raw));
    if (length < 0) {
        return;
    }
    if (server.seen_count < SEEN_LIMIT) {
        seen *entry = &server.log[server.seen_count++];
        sscanf(raw, "%15s %255s", entry->method, entry->path);
        const char *body = strstr(raw, "\r\n\r\n") + 4;
        snprintf(entry->body, sizeof(entry->body), "%s", body);
    }
    if (server.relay_port > 0) {
        relay(client, raw, length);
        return;
    }
    if (strncmp(raw, "GET /api/health ", 16) == 0) {
        send_reply(client, 200, "{\"status\":\"healthy\",\"activeSessions\":0}");
        return;
    }
    if (server.next >= server.script_count) {
        send_reply(client, 500, "{\"success\":false,\"sessionId\":null,"
                                "\"errorMessage\":\"the script ran out\"}");
        return;
    }
    scripted *step = &server.script[server.next++];
    if (step->action == REPLY) {
        send_reply(client, step->status, step->body);
    } else if (step->action == HANG) {
        usleep((useconds_t) step->hang_ms * 1000);
    }
}

static void *serve(void *unused) {
    (void) unused;
    for (;;) {
        int client = accept(server.fd, NULL, NULL);
        if (server.stop) {
            if (client >= 0) {
                close(client);
            }
            return NULL;
        }
        if (client < 0) {
            continue;
        }
        handle(client);
        close(client);
    }
}

static int start_server(int relay_port) {
    memset(&server, 0, sizeof(server));
    server.relay_port = relay_port;
    server.fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(server.fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(server.fd, (struct sockaddr *) &address, sizeof(address));
    listen(server.fd, 16);
    socklen_t size = sizeof(address);
    getsockname(server.fd, (struct sockaddr *) &address, &size);
    server.port = ntohs(address.sin_port);
    pthread_create(&server.thread, NULL, serve, NULL);
    return server.port;
}

static void stop_server(void) {
    if (server.fd < 0) {
        return;
    }
    server.stop = 1;
    /* One last connection wakes an accept that is waiting for one. */
    int wake = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short) server.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(wake, (struct sockaddr *) &address, sizeof(address));
    close(wake);
    pthread_join(server.thread, NULL);
    close(server.fd);
    server.fd = -1;
}

/* ---- the script ------------------------------------------------------------ */

static const char STATUS_SET[] =
    "{\"columns\":[{\"dataType\":\"VARCHAR\",\"name\":\"status\",\"nullable\":false,"
    "\"precision\":0,\"scale\":0}],\"rowCount\":1,"
    "\"rows\":[[\"Statement executed successfully.\"]],\"updateCount\":-1}";

static scripted *add_step(int action) {
    scripted *step = &server.script[server.script_count++];
    step->action = action;
    step->status = 200;
    return step;
}

static const char *number_set(int value) {
    static char text[8][256];
    static int turn;
    char *out = text[turn++ % 8];
    snprintf(out, 256,
             "{\"columns\":[{\"dataType\":\"NUMBER\",\"name\":\"N\",\"nullable\":false,"
             "\"precision\":38,\"scale\":0}],\"rowCount\":1,\"rows\":[[%d]],\"updateCount\":-1}",
             value);
    return out;
}

/* An answer from an engine that reports newSession (0.1.0 and later). */
static void answer(const char *session, int started, const char *set) {
    snprintf(add_step(REPLY)->body, sizeof(server.script[0].body),
             "{\"errorMessage\":null,\"executionTimeMs\":1,\"newSession\":%s,\"resultSets\":[%s],"
             "\"sessionId\":\"%s\",\"success\":true}",
             started ? "true" : "false", set != NULL ? set : STATUS_SET, session);
}

/* An answer from an engine that predates newSession (0.0.7). */
static void legacy(const char *session, const char *set) {
    snprintf(add_step(REPLY)->body, sizeof(server.script[0].body),
             "{\"errorMessage\":null,\"executionTimeMs\":1,\"resultSets\":[%s],"
             "\"sessionId\":\"%s\",\"success\":true}",
             set != NULL ? set : STATUS_SET, session);
}

static void refused(const char *session, const char *message) {
    snprintf(add_step(REPLY)->body, sizeof(server.script[0].body),
             "{\"errorMessage\":\"%s\",\"executionTimeMs\":0,\"newSession\":true,"
             "\"resultSets\":[],\"sessionId\":\"%s\",\"success\":false}",
             message, session);
}

/* The 404 a requireSession request gets when its session is gone. */
static void gone(const char *session) {
    scripted *step = add_step(REPLY);
    step->status = 404;
    snprintf(step->body, sizeof(step->body),
             "{\"errorMessage\":\"Session '%s' does not exist or has expired.\","
             "\"executionTimeMs\":0,\"newSession\":false,\"resultSets\":[],\"sessionId\":null,"
             "\"success\":false}",
             session);
}

static void reply(int status, const char *body) {
    scripted *step = add_step(REPLY);
    step->status = status;
    snprintf(step->body, sizeof(step->body), "%s", body);
}

static void released(void) {
    reply(200, "{\"errorMessage\":null,\"executionTimeMs\":0,\"newSession\":false,"
               "\"resultSets\":[],\"sessionId\":null,\"success\":true}");
}

/* The engine's two answers to the DSN's scope, USE DATABASE APP and USE SCHEMA PUBLIC, the first
 * starting session s1. */
static void scope_answered(void) {
    answer("s1", 1, NULL);
    answer("s1", 0, NULL);
}

/* ---- what the server saw --------------------------------------------------- */

/* The requests of one method, oldest first: their indexes into the log. */
static int requests_of(const char *method, int *out) {
    int count = 0;
    for (int i = 0; i < server.seen_count; i++) {
        if (strcmp(server.log[i].method, method) == 0
            && (strcmp(method, "POST") != 0 || strcmp(server.log[i].path, "/api/execute") == 0)) {
            out[count++] = i;
        }
    }
    return count;
}

/* The `sql` field of an execute request's body. */
static const char *sql_of(int index) {
    static char text[1024];
    const char *field = strstr(server.log[index].body, "\"sql\":\"");
    size_t n = 0;
    if (field != NULL) {
        for (const char *c = field + 7; *c != '\0' && *c != '"' && n + 1 < sizeof(text); c++) {
            if (*c == '\\' && c[1] != '\0') {
                c++;
                text[n++] = *c == 'n' ? '\n' : *c;
            } else {
                text[n++] = *c;
            }
        }
    }
    text[n] = '\0';
    return text;
}

/* Every execute request's SQL, joined with " | ". */
static const char *statements(void) {
    static char joined[4096];
    int executes[SEEN_LIMIT];
    int count = requests_of("POST", executes);
    joined[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            strncat(joined, " | ", sizeof(joined) - strlen(joined) - 1);
        }
        strncat(joined, sql_of(executes[i]), sizeof(joined) - strlen(joined) - 1);
    }
    return joined;
}

static int execute_has(int nth, const char *needle) {
    int executes[SEEN_LIMIT];
    int count = requests_of("POST", executes);
    return nth < count && strstr(server.log[executes[nth]].body, needle) != NULL;
}

static int delete_count(void) {
    int deletes[SEEN_LIMIT];
    return requests_of("DELETE", deletes);
}

static const char *delete_path(int nth) {
    int deletes[SEEN_LIMIT];
    int count = requests_of("DELETE", deletes);
    return nth < count ? server.log[deletes[nth]].path : "";
}

/* ---- the driver -------------------------------------------------------------- */

static struct {
    SQLRETURN (*Alloc)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
    SQLRETURN (*EnvAttr)(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER);
    SQLRETURN (*Connect)(SQLHDBC, SQLHWND, SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                         SQLSMALLINT *, SQLUSMALLINT);
    SQLRETURN (*Exec)(SQLHSTMT, SQLCHAR *, SQLINTEGER);
    SQLRETURN (*Fetch)(SQLHSTMT);
    SQLRETURN (*GetData)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN *);
    SQLRETURN (*Diag)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLCHAR *, SQLINTEGER *, SQLCHAR *,
                      SQLSMALLINT, SQLSMALLINT *);
    SQLRETURN (*SetConnect)(SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER);
    SQLRETURN (*GetConnect)(SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER, SQLINTEGER *);
    SQLRETURN (*EndTran)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT);
    SQLRETURN (*Disconnect)(SQLHDBC);
    SQLRETURN (*Free)(SQLSMALLINT, SQLHANDLE);
} odbc;

static SQLHENV env;

typedef struct outcome {
    SQLRETURN rc;
    char state[6];
    char message[1024];
    char value[256];
} outcome;

static void diag_of(SQLSMALLINT type, SQLHANDLE handle, outcome *out) {
    SQLINTEGER native = 0;
    SQLSMALLINT length = 0;
    odbc.Diag(type, handle, 1, (SQLCHAR *) out->state, &native, (SQLCHAR *) out->message,
              (SQLSMALLINT) sizeof(out->message), &length);
}

/* Connects with the login timeout at one second, which also bounds the release on
 * SQLDisconnect to one second. */
static SQLHDBC open_connection(int port, const char *scope, outcome *out) {
    SQLHDBC dbc = NULL;
    odbc.Alloc(SQL_HANDLE_DBC, env, &dbc);
    odbc.SetConnect(dbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER) 1, 0);
    char text[512];
    snprintf(text, sizeof(text), "Server=127.0.0.1;Port=%d%s", port, scope);
    SQLCHAR buffer[512];
    SQLSMALLINT length = 0;
    outcome ignored;
    if (out == NULL) {
        out = &ignored;
    }
    memset(out, 0, sizeof(*out));
    out->rc = odbc.Connect(dbc, NULL, (SQLCHAR *) text, SQL_NTS, buffer, sizeof(buffer), &length,
                           SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(out->rc)) {
        diag_of(SQL_HANDLE_DBC, dbc, out);
    }
    return dbc;
}

static SQLHDBC open_scoped(void) {
    return open_connection(server.port, ";Database=APP;Schema=PUBLIC", NULL);
}

/* Runs one statement; answers its outcome and the first cell of its first row. */
static outcome run(SQLHDBC dbc, const char *sql) {
    outcome out;
    memset(&out, 0, sizeof(out));
    SQLHSTMT stmt = NULL;
    odbc.Alloc(SQL_HANDLE_STMT, dbc, &stmt);
    out.rc = odbc.Exec(stmt, (SQLCHAR *) sql, SQL_NTS);
    if (SQL_SUCCEEDED(out.rc)) {
        if (odbc.Fetch(stmt) == SQL_SUCCESS) {
            SQLLEN indicator = 0;
            odbc.GetData(stmt, 1, SQL_C_CHAR, out.value, sizeof(out.value), &indicator);
        }
    } else {
        diag_of(SQL_HANDLE_STMT, stmt, &out);
    }
    odbc.Free(SQL_HANDLE_STMT, stmt);
    return out;
}

static void close_connection(SQLHDBC dbc) {
    odbc.Disconnect(dbc);
    odbc.Free(SQL_HANDLE_DBC, dbc);
}

static int lost(const outcome *out, const char *word) {
    return out->rc == SQL_ERROR && strcmp(out->state, "08S01") == 0
        && strstr(out->message, word) != NULL;
}

static double seconds_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double) (now.tv_sec - start->tv_sec) + (double) (now.tv_nsec - start->tv_nsec) / 1e9;
}

#define SCOPE "USE DATABASE APP | USE SCHEMA PUBLIC"

/* ---- the flag ---------------------------------------------------------------- */

static void an_older_engine_is_never_sent_the_flag(void) {
    start_server(0);
    legacy("old1", NULL);
    legacy("old1", NULL);
    legacy("old1", number_set(1));
    SQLHDBC dbc = open_scoped();
    outcome out = run(dbc, "SELECT 1 AS N");
    check(SQL_SUCCEEDED(out.rc) && strcmp(out.value, "1") == 0, "an older engine answers");
    int flagged = 0;
    for (int i = 0; i < 3; i++) {
        flagged += execute_has(i, "requireSession");
    }
    check(flagged == 0, "requireSession is never sent to an engine that predates newSession");
    check(!execute_has(0, "sessionId") && execute_has(2, "\"sessionId\":\"old1\""),
          "the id still travels once the engine has named one");
    close_connection(dbc);
    check(delete_count() == 0, "and SQLDisconnect sends it no DELETE");
    stop_server();
}

static void the_flag_follows_the_first_answer_naming_a_session(void) {
    start_server(0);
    scope_answered();
    answer("s1", 0, number_set(1));
    released();
    SQLHDBC dbc = open_scoped();
    run(dbc, "SELECT 1 AS N");
    check(!execute_has(0, "sessionId") && !execute_has(0, "requireSession"),
          "the first request names no session and requires none");
    check(execute_has(1, "\"sessionId\":\"s1\"") && execute_has(1, "\"requireSession\":true")
              && execute_has(2, "\"sessionId\":\"s1\"") && execute_has(2, "\"requireSession\":true"),
          "every later request names the session and requires it");
    close_connection(dbc);
    stop_server();
}

/* ---- a lost session ------------------------------------------------------------ */

static void a_lost_session_is_replaced_and_the_statement_sent_once_more(void) {
    start_server(0);
    scope_answered();
    gone("s1");
    answer("s2", 1, NULL);
    answer("s2", 0, NULL);
    answer("s2", 0, number_set(1));
    released();
    SQLHDBC dbc = open_scoped();
    outcome out = run(dbc, "SELECT 1 AS N");
    check(SQL_SUCCEEDED(out.rc) && strcmp(out.value, "1") == 0,
          "a lost session is replaced and the statement answers");
    check(strcmp(statements(), SCOPE " | SELECT 1 AS N | " SCOPE " | SELECT 1 AS N") == 0,
          "the scope goes on a fresh session, and the statement is sent once more");
    check(!execute_has(3, "sessionId") && execute_has(5, "\"sessionId\":\"s2\""),
          "the fresh session starts without an id, and the statement goes in the new one");
    close_connection(dbc);
    check(delete_count() == 1 && strcmp(delete_path(0), "/api/sessions/s2") == 0,
          "and it is the new session that SQLDisconnect releases");
    stop_server();
}

static void a_session_lost_twice_is_reported(void) {
    start_server(0);
    scope_answered();
    gone("s1");
    answer("s2", 1, NULL);
    answer("s2", 0, NULL);
    gone("s2");
    answer("s3", 1, NULL);
    answer("s3", 0, NULL);
    answer("s3", 0, number_set(2));
    released();
    SQLHDBC dbc = open_scoped();
    outcome out = run(dbc, "SELECT 1 AS N");
    check(lost(&out, "just started"), "a session lost again straight away is 08S01");
    int executes[SEEN_LIMIT];
    int count = requests_of("POST", executes);
    int sent = 0;
    for (int i = 0; i < count; i++) {
        sent += strcmp(sql_of(executes[i]), "SELECT 1 AS N") == 0;
    }
    check(sent == 2, "the statement went twice, never a third time");
    out = run(dbc, "SELECT 2 AS N");
    check(SQL_SUCCEEDED(out.rc) && strcmp(out.value, "2") == 0,
          "and the connection goes on with a fresh session");
    close_connection(dbc);
    stop_server();
}

static void a_lost_transaction_is_reported_not_replaced(void) {
    start_server(0);
    scope_answered();
    answer("s1", 0, NULL);
    gone("s1");
    answer("s2", 1, NULL);
    answer("s2", 0, NULL);
    answer("s2", 0, number_set(1));
    released();
    SQLHDBC dbc = open_scoped();
    run(dbc, "BEGIN");
    outcome out = run(dbc, "INSERT INTO t VALUES (1)");
    check(lost(&out, "transaction"), "a lost session that held a transaction is 08S01");
    check(strcmp(statements(), SCOPE " | BEGIN | INSERT INTO t VALUES (1)") == 0,
          "and the statement is not sent again");
    out = run(dbc, "SELECT 1 AS N");
    check(SQL_SUCCEEDED(out.rc) && strcmp(out.value, "1") == 0,
          "the next statement runs in a fresh session");
    check(strcmp(statements(),
                 SCOPE " | BEGIN | INSERT INTO t VALUES (1) | " SCOPE " | SELECT 1 AS N") == 0,
          "on the DSN's scope");
    close_connection(dbc);
    stop_server();
}

static void manual_commit_mode_holds_a_transaction(void) {
    start_server(0);
    scope_answered();
    answer("s1", 0, NULL); /* ALTER SESSION SET AUTOCOMMIT = FALSE */
    answer("s1", 0, NULL); /* the first INSERT */
    gone("s1");
    answer("s2", 1, NULL);
    answer("s2", 0, NULL);
    answer("s2", 0, NULL); /* the scope puts AUTOCOMMIT off again */
    answer("s2", 0, number_set(1));
    released();
    SQLHDBC dbc = open_scoped();
    check(SQL_SUCCEEDED(odbc.SetConnect(dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER) SQL_AUTOCOMMIT_OFF, 0)),
          "SQL_ATTR_AUTOCOMMIT goes off");
    run(dbc, "INSERT INTO t VALUES (1)");
    outcome out = run(dbc, "INSERT INTO t VALUES (2)");
    check(lost(&out, "transaction"),
          "with autocommit off, a lost session took the uncommitted work with it: 08S01");
    out = run(dbc, "SELECT 1 AS N");
    check(SQL_SUCCEEDED(out.rc) && strcmp(out.value, "1") == 0, "the next statement runs");
    check(strcmp(statements(),
                 SCOPE " | ALTER SESSION SET AUTOCOMMIT = FALSE | INSERT INTO t VALUES (1)"
                       " | INSERT INTO t VALUES (2) | " SCOPE
                       " | ALTER SESSION SET AUTOCOMMIT = FALSE | SELECT 1 AS N") == 0,
          "in a fresh session put back in manual-commit mode");
    close_connection(dbc);
    stop_server();
}

static void manual_commit_mode_set_before_connecting_is_part_of_the_scope(void) {
    start_server(0);
    scope_answered();
    answer("s1", 0, NULL); /* ALTER SESSION SET AUTOCOMMIT = FALSE, while connecting */
    answer("s1", 0, NULL); /* INSERT */
    gone("s1");
    released();
    SQLHDBC dbc = NULL;
    odbc.Alloc(SQL_HANDLE_DBC, env, &dbc);
    odbc.SetConnect(dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER) SQL_AUTOCOMMIT_OFF, 0);
    odbc.SetConnect(dbc, SQL_ATTR_LOGIN_TIMEOUT, (SQLPOINTER) 1, 0);
    char text[256];
    snprintf(text, sizeof(text), "Server=127.0.0.1;Port=%d;Database=APP;Schema=PUBLIC", server.port);
    SQLCHAR buffer[512];
    SQLSMALLINT length = 0;
    check(SQL_SUCCEEDED(odbc.Connect(dbc, NULL, (SQLCHAR *) text, SQL_NTS, buffer, sizeof(buffer),
                                     &length, SQL_DRIVER_NOPROMPT)),
          "a connection with SQL_ATTR_AUTOCOMMIT off before connecting opens");
    check(strcmp(statements(), SCOPE " | ALTER SESSION SET AUTOCOMMIT = FALSE") == 0,
          "on a session in manual-commit mode from the start");
    run(dbc, "INSERT INTO t VALUES (1)");
    outcome out = run(dbc, "SELECT 1");
    check(lost(&out, "transaction"), "whose uncommitted work a lost session takes with it");
    close_connection(dbc);
    stop_server();
}

static void manual_commit_mode_after_a_commit_recovers(void) {
    start_server(0);
    scope_answered();
    answer("s1", 0, NULL); /* ALTER SESSION SET AUTOCOMMIT = FALSE */
    answer("s1", 0, NULL); /* INSERT */
    answer("s1", 0, NULL); /* COMMIT */
    gone("s1");
    answer("s2", 1, NULL);
    answer("s2", 0, NULL);
    answer("s2", 0, NULL);
    answer("s2", 0, number_set(1));
    released();
    SQLHDBC dbc = open_scoped();
    odbc.SetConnect(dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER) SQL_AUTOCOMMIT_OFF, 0);
    run(dbc, "INSERT INTO t VALUES (1)");
    check(SQL_SUCCEEDED(odbc.EndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT)), "SQLEndTran commits");
    outcome out = run(dbc, "SELECT 1 AS N");
    check(SQL_SUCCEEDED(out.rc) && strcmp(out.value, "1") == 0,
          "with nothing uncommitted, a lost session is replaced");
    check(strcmp(statements(),
                 SCOPE " | ALTER SESSION SET AUTOCOMMIT = FALSE | INSERT INTO t VALUES (1)"
                       " | COMMIT | SELECT 1 AS N | " SCOPE
                       " | ALTER SESSION SET AUTOCOMMIT = FALSE | SELECT 1 AS N") == 0,
          "back in manual-commit mode before the statement goes again");
    close_connection(dbc);
    stop_server();
}

static void a_lost_context_is_reported_not_replaced(void) {
    static const char *const movers[] = {
        "USE SCHEMA other", "SET v = 1", "UNSET v", "ALTER SESSION SET TIMEZONE = 'UTC'",
        "CREATE TEMPORARY TABLE tmp (i INT)", "CREATE DATABASE other",
        "SELECT 1; USE SCHEMA other", NULL
    };
    int reported = 0;
    int resent = 0;
    int total = 0;
    for (int i = 0; movers[i] != NULL; i++, total++) {
        start_server(0);
        scope_answered();
        answer("s1", 0, NULL);
        gone("s1");
        SQLHDBC dbc = open_scoped();
        run(dbc, movers[i]);
        outcome out = run(dbc, "SELECT * FROM t");
        if (lost(&out, "context")) {
            reported++;
        } else {
            fprintf(stderr, "  %s: %s %s\n", movers[i], out.state, out.message);
        }
        char expected[512];
        snprintf(expected, sizeof(expected), SCOPE " | %s | SELECT * FROM t", movers[i]);
        resent += strcmp(statements(), expected) != 0;
        close_connection(dbc);
        stop_server();
    }
    check(reported == total,
          "USE, SET, UNSET, ALTER SESSION, a temporary object or CREATE DATABASE went with the session: 08S01");
    check(resent == 0, "and the statement is not sent again");
}

static void a_catalog_set_through_the_attribute_is_context_too(void) {
    start_server(0);
    scope_answered();
    answer("s1", 0, NULL); /* USE DATABASE OTHER */
    gone("s1");
    SQLHDBC dbc = open_scoped();
    check(SQL_SUCCEEDED(odbc.SetConnect(dbc, SQL_ATTR_CURRENT_CATALOG, (SQLPOINTER) "OTHER", SQL_NTS)),
          "SQL_ATTR_CURRENT_CATALOG moves the session");
    outcome out = run(dbc, "SELECT 1");
    check(lost(&out, "context"), "a lost session on another catalog is 08S01, not re-run on the DSN's");
    char catalog[64] = "";
    SQLINTEGER length = 0;
    odbc.GetConnect(dbc, SQL_ATTR_CURRENT_CATALOG, catalog, sizeof(catalog), &length);
    check(strcmp(catalog, "APP") == 0, "and the current catalog is the DSN's again");
    close_connection(dbc);
    stop_server();
}

static void a_replaced_session_gets_the_scope_back(void) {
    start_server(0);
    scope_answered();
    answer("s1", 1, number_set(1)); /* the engine ran it in a fresh session under our id */
    answer("s1", 0, NULL);
    answer("s1", 0, NULL);
    answer("s1", 0, number_set(2));
    released();
    SQLHDBC dbc = open_scoped();
    run(dbc, "SELECT 1 AS N");
    outcome out = run(dbc, "SELECT 2 AS N");
    check(SQL_SUCCEEDED(out.rc) && strcmp(out.value, "2") == 0, "a replaced session answers");
    check(strcmp(statements(), SCOPE " | SELECT 1 AS N | " SCOPE " | SELECT 2 AS N") == 0,
          "with the scope back on before the next statement");
    close_connection(dbc);
    stop_server();
}

/* ---- disconnecting ------------------------------------------------------------ */

static void disconnect_releases_the_session_once(void) {
    start_server(0);
    scope_answered();
    answer("s1", 0, NULL);
    released();
    SQLHDBC dbc = open_scoped();
    run(dbc, "BEGIN");
    check(odbc.Disconnect(dbc) == SQL_SUCCESS, "SQLDisconnect succeeds");
    check(delete_count() == 1 && strcmp(delete_path(0), "/api/sessions/s1") == 0,
          "and releases the session with one DELETE");
    check(odbc.Disconnect(dbc) == SQL_ERROR && delete_count() == 1,
          "a second SQLDisconnect is refused and sends nothing");
    odbc.Free(SQL_HANDLE_DBC, dbc);
    stop_server();
}

static void disconnect_never_fails_on_the_release(void) {
    static const char *const names[] = {
        "a 404", "a 405", "a socket closed without a reply", "a server that never answers",
        "a server that has gone"
    };
    int clean = 0;
    for (int i = 0; i < 5; i++) {
        start_server(0);
        scope_answered();
        switch (i) {
            case 0: reply(404, "{\"success\":false,\"sessionId\":null}"); break;
            case 1: reply(405, "{\"error\":\"Method not allowed\"}"); break;
            case 2: add_step(CLOSE); break;
            case 3: add_step(HANG)->hang_ms = 2500; break;
            default: break;
        }
        SQLHDBC dbc = open_scoped();
        if (i == 4) {
            stop_server();
        }
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        SQLRETURN rc = odbc.Disconnect(dbc);
        double took = seconds_since(&start);
        int deletes = i == 4 ? 1 : delete_count();
        if (rc == SQL_SUCCESS && took < 2.0 && deletes == 1) {
            clean++;
        } else {
            fprintf(stderr, "  %s: rc %d after %.2fs, %d DELETE(s)\n", names[i], rc, took, deletes);
        }
        odbc.Free(SQL_HANDLE_DBC, dbc);
        stop_server();
    }
    check(clean == 5,
          "SQLDisconnect succeeds, within the connection's own timeout, whatever the release meets");
}

static void a_refused_scope_releases_the_session(void) {
    start_server(0);
    refused("s9", "Database 'APP' does not exist or not authorized.");
    released();
    outcome out;
    SQLHDBC dbc = open_connection(server.port, ";Database=APP;Schema=PUBLIC", &out);
    check(out.rc == SQL_ERROR && strcmp(out.state, "08004") == 0, "a scope the engine refuses fails the connect");
    check(delete_count() == 1 && strcmp(delete_path(0), "/api/sessions/s9") == 0,
          "and the session the refusal ran in is released");
    odbc.Free(SQL_HANDLE_DBC, dbc);
    stop_server();
}

/* ---- a live engine ------------------------------------------------------------- */

/* One raw request to the engine: answers its status, the body copied into `body`. */
static int engine_request(int port, const char *method, const char *path, char *body, size_t size) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short) port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    set_timeout(fd, 30);
    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    char request[512];
    snprintf(request, sizeof(request),
             "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
             method, path, port);
    write_all(fd, request, strlen(request));
    char answer[8192];
    size_t got = 0;
    for (;;) {
        ssize_t n = recv(fd, answer + got, sizeof(answer) - got - 1, 0);
        if (n <= 0) {
            break;
        }
        got += (size_t) n;
    }
    close(fd);
    answer[got] = '\0';
    int status = -1;
    sscanf(answer, "HTTP/1.%*d %d", &status);
    const char *payload = strstr(answer, "\r\n\r\n");
    snprintf(body, size, "%s", payload != NULL ? payload + 4 : "");
    return status;
}

static long active_sessions(int port) {
    char body[512];
    engine_request(port, "GET", "/api/sessions", body, sizeof(body));
    const char *field = strstr(body, "\"activeSessions\":");
    return field != NULL ? strtol(field + 17, NULL, 10) : -1;
}

/* Releases the relayed connection's session the way an expiry or a restart would lose it. */
static int release_behind_its_back(int port) {
    char path[256];
    char body[512];
    snprintf(path, sizeof(path), "/api/sessions/%s", server.relayed_session);
    return engine_request(port, "DELETE", path, body, sizeof(body));
}

#define LIVE_SCOPE ";Database=odbc_session_db;Schema=odbc_session_schema"

static void against_a_live_engine(int engine) {
    SQLHDBC setup = open_connection(engine, "", NULL);
    run(setup, "CREATE DATABASE IF NOT EXISTS odbc_session_db");
    run(setup, "USE DATABASE odbc_session_db");
    run(setup, "CREATE SCHEMA IF NOT EXISTS odbc_session_schema");

    start_server(engine);
    SQLHDBC dbc = open_connection(server.port, LIVE_SCOPE, NULL);
    run(dbc, "SELECT 1");
    char first[128];
    snprintf(first, sizeof(first), "%s", server.relayed_session);
    check(release_behind_its_back(engine) == 200, "the engine releases the session out of band");
    outcome database = run(dbc, "SELECT CURRENT_DATABASE()");
    outcome schema = run(dbc, "SELECT CURRENT_SCHEMA()");
    check(SQL_SUCCEEDED(database.rc) && strcmp(database.value, "ODBC_SESSION_DB") == 0
              && strcmp(schema.value, "ODBC_SESSION_SCHEMA") == 0,
          "the next statement runs in a fresh session on the DSN's scope");
    check(strcmp(first, server.relayed_session) != 0, "a fresh session took over");
    close_connection(dbc);

    dbc = open_connection(server.port, LIVE_SCOPE, NULL);
    run(dbc, "CREATE OR REPLACE TABLE lost_tx (i INT)");
    run(dbc, "BEGIN");
    run(dbc, "INSERT INTO lost_tx VALUES (1)");
    release_behind_its_back(engine);
    outcome out = run(dbc, "INSERT INTO lost_tx VALUES (2)");
    check(lost(&out, "transaction"), "a released session that held a transaction is 08S01");
    out = run(dbc, "SELECT COUNT(*) FROM lost_tx");
    check(SQL_SUCCEEDED(out.rc) && strcmp(out.value, "0") == 0,
          "the release rolled the first insert back, and the second never ran");
    close_connection(dbc);
    stop_server();

    long before = active_sessions(engine);
    dbc = open_connection(engine, "", NULL);
    run(dbc, "SELECT 1");
    long during = active_sessions(engine);
    close_connection(dbc);
    long after = active_sessions(engine);
    check(during == before + 1 && after == before, "SQLDisconnect lowers activeSessions by one");

    run(setup, "DROP DATABASE IF EXISTS odbc_session_db");
    close_connection(setup);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: session <driver.so>\n");
        return 2;
    }
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (lib == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }
    *(void **) &odbc.Alloc = dlsym(lib, "SQLAllocHandle");
    *(void **) &odbc.EnvAttr = dlsym(lib, "SQLSetEnvAttr");
    *(void **) &odbc.Connect = dlsym(lib, "SQLDriverConnect");
    *(void **) &odbc.Exec = dlsym(lib, "SQLExecDirect");
    *(void **) &odbc.Fetch = dlsym(lib, "SQLFetch");
    *(void **) &odbc.GetData = dlsym(lib, "SQLGetData");
    *(void **) &odbc.Diag = dlsym(lib, "SQLGetDiagRec");
    *(void **) &odbc.SetConnect = dlsym(lib, "SQLSetConnectAttr");
    *(void **) &odbc.GetConnect = dlsym(lib, "SQLGetConnectAttr");
    *(void **) &odbc.EndTran = dlsym(lib, "SQLEndTran");
    *(void **) &odbc.Disconnect = dlsym(lib, "SQLDisconnect");
    *(void **) &odbc.Free = dlsym(lib, "SQLFreeHandle");
    if (!odbc.Alloc || !odbc.EnvAttr || !odbc.Connect || !odbc.Exec || !odbc.Fetch
        || !odbc.GetData || !odbc.Diag || !odbc.SetConnect || !odbc.GetConnect || !odbc.EndTran
        || !odbc.Disconnect || !odbc.Free) {
        fprintf(stderr, "the driver does not export the ODBC entry points\n");
        return 2;
    }
    odbc.Alloc(SQL_HANDLE_ENV, NULL, &env);
    odbc.EnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER) SQL_OV_ODBC3, 0);
    server.fd = -1;

    an_older_engine_is_never_sent_the_flag();
    the_flag_follows_the_first_answer_naming_a_session();
    a_lost_session_is_replaced_and_the_statement_sent_once_more();
    a_session_lost_twice_is_reported();
    a_lost_transaction_is_reported_not_replaced();
    manual_commit_mode_holds_a_transaction();
    manual_commit_mode_set_before_connecting_is_part_of_the_scope();
    manual_commit_mode_after_a_commit_recovers();
    a_lost_context_is_reported_not_replaced();
    a_catalog_set_through_the_attribute_is_context_too();
    a_replaced_session_gets_the_scope_back();
    disconnect_releases_the_session_once();
    disconnect_never_fails_on_the_release();
    a_refused_scope_releases_the_session();

    const char *port = getenv("FROSTLAKE_TEST_PORT");
    if (port != NULL && port[0] != '\0') {
        against_a_live_engine(atoi(port));
    } else {
        printf("# skipping the live-engine checks: FROSTLAKE_TEST_PORT is not set\n");
    }

    odbc.Free(SQL_HANDLE_ENV, env);
    if (failures > 0) {
        fprintf(stderr, "%d of %d session checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d session checks passed\n", checks);
    return 0;
}
