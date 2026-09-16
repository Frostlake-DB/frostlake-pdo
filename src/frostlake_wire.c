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
 * The engine's HTTP protocol: POST /api/execute, GET /api/health, DELETE /api/sessions/{id}.
 *
 * The server answers a SQL failure with success=false and an errorMessage, and keeps the session
 * it ran in, so only an unreachable server or an unreadable answer is a transport failure.
 */

#include "php.h"

#include "php_pdo_frostlake_int.h"
#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A JSON string of exactly `length` bytes. Unlike fl_strbuf_append_json_string this does not stop
 * at a NUL, which a bound PHP string may well contain. */
static int append_json_string(fl_strbuf *buf, const char *text, size_t length) {
    if (fl_strbuf_append(buf, "\"") != 0) {
        return -1;
    }
    size_t run = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char) text[i];
        const char *escape = NULL;
        char control[7];
        switch (c) {
            case '"':  escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default:
                if (c < 0x20) {
                    snprintf(control, sizeof(control), "\\u%04x", c);
                    escape = control;
                }
        }
        if (escape != NULL) {
            if (fl_strbuf_append_len(buf, text + run, i - run) != 0 || fl_strbuf_append(buf, escape) != 0) {
                return -1;
            }
            run = i + 1;
        }
    }
    if (fl_strbuf_append_len(buf, text + run, length - run) != 0) {
        return -1;
    }
    return fl_strbuf_append(buf, "\"");
}

/* Whether a transport failure is the read timeout expiring: the socket gave up waiting. */
static bool timed_out(const char *transport_error) {
    return transport_error != NULL && strstr(transport_error, "cannot read response") != NULL
        && (strstr(transport_error, strerror(EAGAIN)) != NULL
            || strstr(transport_error, strerror(ETIMEDOUT)) != NULL);
}

static char *build_request(const pdo_frostlake_db_handle *H, const char *sql, size_t sql_length,
                           zend_long multi_statement_count) {
    fl_strbuf buf;
    fl_strbuf_init(&buf);
    int failed = fl_strbuf_append(&buf, "{\"sql\":") != 0
        || append_json_string(&buf, sql, sql_length) != 0;
    if (!failed && H->session_id != NULL) {
        /* A session that has gone away — expired, released, or lost to a server restart — would
         * otherwise be replaced by a fresh one without the context this connection set up. */
        failed = fl_strbuf_append(&buf, ",\"sessionId\":") != 0
            || fl_strbuf_append_json_string(&buf, H->session_id) != 0
            || fl_strbuf_append(&buf, ",\"requireSession\":true") != 0;
    }
    if (!failed && multi_statement_count >= 0) {
        char declared[64];
        snprintf(declared, sizeof(declared), ",\"multiStatementCount\":" ZEND_LONG_FMT, multi_statement_count);
        failed = fl_strbuf_append(&buf, declared) != 0;
    }
    if (!failed) {
        failed = fl_strbuf_append(&buf, H->opened_auto_commit ? ",\"autoCommit\":true}" : ",\"autoCommit\":false}") != 0;
    }
    if (failed) {
        fl_strbuf_free(&buf);
        return NULL;
    }
    return buf.data;
}

static void pin_session(pdo_dbh_t *dbh, const fl_json *response) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    const char *session_id = fl_json_get_string(response, "sessionId");
    if (session_id == NULL || (H->session_id != NULL && strcmp(H->session_id, session_id) == 0)) {
        return;
    }
    if (H->session_id != NULL) {
        pefree(H->session_id, dbh->is_persistent);
    }
    H->session_id = pestrdup(session_id, dbh->is_persistent);
}

fl_json *pdo_frostlake_execute(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *sql, size_t sql_length,
                               zend_long multi_statement_count) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    if (H == NULL) {
        pdo_frostlake_error(dbh, stmt, "08003", "The connection is closed");
        return NULL;
    }

    char *request = build_request(H, sql, sql_length, multi_statement_count);
    if (request == NULL) {
        pdo_frostlake_error(dbh, stmt, "HY001", "Out of memory building the request");
        return NULL;
    }

    int status = 0;
    char *body = NULL;
    char *transport_error = NULL;
    int rc = fl_http_post(H->host, H->port, "/api/execute", request, H->timeout,
                          &status, &body, &transport_error);
    free(request);
    if (rc != 0) {
        char *message;
        if (timed_out(transport_error)) {
            spprintf(&message, 0, "The Frostlake server at %s:%d did not answer within %d second%s",
                     H->host, H->port, H->timeout, H->timeout == 1 ? "" : "s");
            pdo_frostlake_error(dbh, stmt, "HYT00", message);
        } else {
            spprintf(&message, 0, "Cannot reach the Frostlake server at %s:%d: %s", H->host, H->port,
                     transport_error != NULL ? transport_error : "transport failure");
            pdo_frostlake_error(dbh, stmt, "08S01", message);
        }
        free(transport_error);
        efree(message);
        return NULL;
    }

    fl_json *response = fl_json_parse(body, strlen(body));
    if (response == NULL || response->kind != FL_JSON_OBJECT) {
        char *message;
        spprintf(&message, 0, "Unreadable answer from the Frostlake server (HTTP %d): %.200s", status, body);
        free(body);
        fl_json_free(response);
        pdo_frostlake_error(dbh, stmt, "08S01", message);
        efree(message);
        return NULL;
    }
    free(body);

    if (!fl_json_get_bool(response, "success", 0)) {
        const char *error_message = fl_json_get_string(response, "errorMessage");
        if (error_message == NULL) {
            /* the shape of a server fault that never reached a statement */
            error_message = fl_json_get_string(response, "error");
        }
        if (status == 404 && H->session_id != NULL) {
            /* requireSession refused: nothing ran, and the session this connection set up is
             * gone. */
            char *message;
            spprintf(&message, 0, "The Frostlake session of this connection no longer exists: %s",
                     error_message != NULL ? error_message : "session not found");
            fl_json_free(response);
            pdo_frostlake_error(dbh, stmt, "08003", message);
            efree(message);
            return NULL;
        }
        pin_session(dbh, response);
        pdo_frostlake_error(dbh, stmt, status == 200 || status == 400 ? "HY000" : "08S01",
                            error_message != NULL ? error_message : "The statement failed");
        fl_json_free(response);
        return NULL;
    }

    pin_session(dbh, response);
    return response;
}

bool pdo_frostlake_healthy(pdo_dbh_t *dbh) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    int status = 0;
    char *body = NULL;
    char *transport_error = NULL;
    /* A statement limit of 0 means none; the check keeps its own. */
    int timeout = H->timeout > 0 && H->timeout < PDO_FROSTLAKE_CONNECT_TIMEOUT ? H->timeout : PDO_FROSTLAKE_CONNECT_TIMEOUT;
    int rc = fl_http_get(H->host, H->port, "/api/health", timeout, &status, &body, &transport_error);
    if (rc != 0 || status != 200) {
        char *message;
        if (rc != 0) {
            spprintf(&message, 0, "Cannot reach the Frostlake server at %s:%d: %s", H->host, H->port,
                     transport_error != NULL ? transport_error : "transport failure");
        } else {
            spprintf(&message, 0, "The Frostlake server at %s:%d is not healthy (HTTP %d)",
                     H->host, H->port, status);
        }
        free(transport_error);
        free(body);
        pdo_frostlake_error(dbh, NULL, "08001", message);
        efree(message);
        return false;
    }
    free(body);
    return true;
}

void pdo_frostlake_release_session(pdo_frostlake_db_handle *H) {
    if (H->session_id == NULL) {
        return;
    }
    /* Session ids are the server's own UUID-shaped tokens; anything else is not sent. */
    for (const char *c = H->session_id; *c != '\0'; c++) {
        if (!(isalnum((unsigned char) *c) || *c == '-' || *c == '_')) {
            return;
        }
    }
    fl_strbuf path;
    fl_strbuf_init(&path);
    if (fl_strbuf_append(&path, "/api/sessions/") == 0 && fl_strbuf_append(&path, H->session_id) == 0) {
        int status = 0;
        char *body = NULL;
        char *transport_error = NULL;
        /* Best effort: an unreachable server has already dropped the session with everything
         * else, and an idle session expires by itself. */
        fl_http_delete(H->host, H->port, path.data, PDO_FROSTLAKE_CONNECT_TIMEOUT,
                       &status, &body, &transport_error);
        free(body);
        free(transport_error);
    }
    fl_strbuf_free(&path);
}
