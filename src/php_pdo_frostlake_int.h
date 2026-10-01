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

#ifndef PHP_PDO_FROSTLAKE_INT_H
#define PHP_PDO_FROSTLAKE_INT_H

#include "php.h"
#include "ext/pdo/php_pdo.h"
#include "ext/pdo/php_pdo_driver.h"

#include "php_pdo_frostlake.h"
#include "json.h"

/* The driver-specific attribute. Its number is the one pdo_snowflake gives
 * PDO::SNOWFLAKE_STMT_MULTI_STMT_COUNT, so code that passes the number, or picks the constant by
 * driver name, means the same thing under either driver. */
enum {
    PDO_FROSTLAKE_ATTR_STMT_MULTI_STMT_COUNT = PDO_ATTR_DRIVER_SPECIFIC + 4
};

/* No count declared: the request leaves the field out and the session's MULTI_STATEMENT_COUNT
 * decides. */
#define PDO_FROSTLAKE_COUNT_UNSET (-1)

#define PDO_FROSTLAKE_DEFAULT_PORT 18082
/* Seconds a statement may take, and seconds the connection check may wait for the server. */
#define PDO_FROSTLAKE_DEFAULT_TIMEOUT 300
#define PDO_FROSTLAKE_CONNECT_TIMEOUT 10
/* The data source's USE statements: role, warehouse, database and schema. */
#define PDO_FROSTLAKE_SCOPE_MAX 4

typedef struct {
    char *host;
    int port;
    int timeout;
    /* The server-assigned session, pinned by the first answer that names one. */
    char *session_id;
    /* Whether the engine reports newSession, which arrived together with requireSession and
     * DELETE /api/sessions: -1 until the first answer that names a session settles it. */
    int tracks_sessions;
    /* What the session holds that a fresh one would not: context a statement set up (USE, SET,
     * ALTER SESSION, a temporary object, CREATE or DROP of a database or schema), and an open
     * transaction. */
    bool dirty;
    bool in_transaction;
    /* The data source's USE statements, in the order a login applies them, which every session of
     * the connection starts with; and whether the engine must accept each (the role) or may refuse
     * it (the rest, as a Snowflake login leaves off one that does not exist). */
    char *scope[PDO_FROSTLAKE_SCOPE_MAX];
    bool scope_required[PDO_FROSTLAKE_SCOPE_MAX];
    int scope_count;
    /* Set once a lost session was dropped: the scope goes onto a fresh session before the next
     * statement. */
    bool scope_pending;
    /* The connection's PDO::FROSTLAKE_STMT_MULTI_STMT_COUNT, copied into each statement it
     * prepares. */
    zend_long multi_statement_count;
    /* The autocommit mode the connection was opened with — what every request sends. */
    bool opened_auto_commit;
    /* The message of the connection's last failure. */
    char *errmsg;
} pdo_frostlake_db_handle;

typedef struct {
    pdo_frostlake_db_handle *H;
    /* The last execution's answer; everything below points into it. */
    fl_json *response;
    size_t result_set;
    const fl_json *columns;
    const fl_json *rows;
    /* The row the cursor stands on; -1 before the first fetch. */
    zend_long row;
    zend_long multi_statement_count;
    char *errmsg;
} pdo_frostlake_stmt;

extern const pdo_driver_t pdo_frostlake_driver;
extern const struct pdo_stmt_methods pdo_frostlake_stmt_methods;

/* Records a failure on the statement, or on the connection when stmt is NULL. While the
 * connection is still being opened there is nobody to report to later, so it throws. */
void pdo_frostlake_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *sqlstate, const char *message);

/* Sends one request and answers its parsed response, or NULL after recording why. A session the
 * engine no longer holds is replaced by a fresh one on the data source's scope and the request
 * sent once more, unless the lost session held a transaction or context of its own: that is
 * refused with 08003. */
fl_json *pdo_frostlake_execute(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *sql, size_t sql_length,
                               zend_long multi_statement_count);
/* Whether the connection's session is still there to use, without replacing it: a lost session
 * that held a transaction or context of its own is left for the next statement to report. */
bool pdo_frostlake_session_alive(pdo_dbh_t *dbh);
/* Whether the server answers its health check; a failure is recorded on the connection. */
bool pdo_frostlake_healthy(pdo_dbh_t *dbh);
void pdo_frostlake_release_session(pdo_frostlake_db_handle *H);

/* The statement's SQL with its bound parameters substituted, or NULL with *sqlstate and an
 * emalloc'd *message set. */
zend_string *pdo_frostlake_render_statement(pdo_stmt_t *stmt, const char **sqlstate, char **message);
int pdo_frostlake_is_identifier(const char *text);
int pdo_frostlake_append_json(fl_strbuf *buf, const fl_json *node);
/* Keeps H's picture of its session in step with a request that succeeded: whether a statement in
 * it set up context a fresh session would not have, and what it did to the transaction — which,
 * with autocommit off, any statement but COMMIT or ROLLBACK may leave open. */
void pdo_frostlake_track_session(pdo_frostlake_db_handle *H, const char *sql, size_t length);

/* The affected-row count of a result set: its update count for DML, its row count otherwise. */
zend_long pdo_frostlake_result_row_count(const fl_json *result_set);

#endif /* PHP_PDO_FROSTLAKE_INT_H */
