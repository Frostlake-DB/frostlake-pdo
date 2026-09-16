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
 * The connection: opening it, the PDO connection methods, and error reporting.
 *
 * The surface follows pdo_snowflake, the account's own PDO driver, so that PHP code tested against
 * Frostlake meets what it will meet against Snowflake — including what that driver leaves out.
 */

#include "php.h"

#include "php_pdo_frostlake.h"
#include "php_pdo_frostlake_int.h"

#include <stdlib.h>
#include <string.h>

void pdo_frostlake_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *sqlstate, const char *message) {
    if (stmt != NULL) {
        pdo_frostlake_stmt *S = stmt->driver_data;
        strncpy(stmt->error_code, sqlstate, sizeof(pdo_error_type) - 1);
        stmt->error_code[sizeof(pdo_error_type) - 1] = '\0';
        if (S->errmsg != NULL) {
            efree(S->errmsg);
        }
        S->errmsg = estrdup(message);
        return;
    }

    pdo_frostlake_db_handle *H = dbh->driver_data;
    strncpy(dbh->error_code, sqlstate, sizeof(pdo_error_type) - 1);
    dbh->error_code[sizeof(pdo_error_type) - 1] = '\0';
    if (H != NULL) {
        if (H->errmsg != NULL) {
            pefree(H->errmsg, dbh->is_persistent);
        }
        H->errmsg = pestrdup(message, dbh->is_persistent);
    }
    if (dbh->methods == NULL) {
        /* Still connecting: the constructor reports nothing by itself, so throw. */
        pdo_throw_exception(0, (char *) message, &dbh->error_code);
    }
}

static void clear_error(pdo_dbh_t *dbh) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    if (H != NULL && H->errmsg != NULL) {
        pefree(H->errmsg, dbh->is_persistent);
        H->errmsg = NULL;
    }
}

static void free_handle(pdo_dbh_t *dbh) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    if (H == NULL) {
        return;
    }
    if (H->host != NULL) {
        pefree(H->host, dbh->is_persistent);
    }
    if (H->session_id != NULL) {
        pefree(H->session_id, dbh->is_persistent);
    }
    if (H->errmsg != NULL) {
        pefree(H->errmsg, dbh->is_persistent);
    }
    pefree(H, dbh->is_persistent);
    dbh->driver_data = NULL;
}

static void pdo_frostlake_handle_closer(pdo_dbh_t *dbh) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    if (H != NULL) {
        /* Releasing the session rolls back a transaction it left open, as closing a Snowflake
         * session does. */
        pdo_frostlake_release_session(H);
        free_handle(dbh);
    }
}

static bool pdo_frostlake_handle_preparer(pdo_dbh_t *dbh, zend_string *sql, pdo_stmt_t *stmt,
                                          zval *driver_options) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    if (H == NULL) {
        pdo_frostlake_error(dbh, NULL, "08003", "The connection is closed");
        return false;
    }
    pdo_frostlake_stmt *S = ecalloc(1, sizeof(pdo_frostlake_stmt));

    S->H = H;
    S->row = -1;
    /* The count is the connection's at the moment of preparing, as pdo_snowflake takes it. */
    S->multi_statement_count = H->multi_statement_count;

    stmt->driver_data = S;
    stmt->methods = &pdo_frostlake_stmt_methods;
    /* Placeholders are substituted by this driver, not parsed by PDO: PDO's parser would take the
     * VARIANT path in v:field for a named parameter. */
    stmt->supports_placeholders = PDO_PLACEHOLDER_NAMED | PDO_PLACEHOLDER_POSITIONAL;
    return true;
}

static zend_long pdo_frostlake_handle_doer(pdo_dbh_t *dbh, const zend_string *sql) {
    clear_error(dbh);
    /* PDO::exec sends no statement count — pdo_snowflake runs it on a statement of its own that
     * never takes the connection's — so the session's MULTI_STATEMENT_COUNT decides. */
    fl_json *response = pdo_frostlake_execute(dbh, NULL, ZSTR_VAL(sql), ZSTR_LEN(sql), PDO_FROSTLAKE_COUNT_UNSET);
    if (response == NULL) {
        return -1;
    }
    const fl_json *first = fl_json_at(fl_json_get(response, "resultSets"), 0);
    zend_long count = first != NULL ? pdo_frostlake_result_row_count(first) : 0;
    fl_json_free(response);
    return count;
}

static bool run_transaction_statement(pdo_dbh_t *dbh, const char *sql) {
    clear_error(dbh);
    fl_json *response = pdo_frostlake_execute(dbh, NULL, sql, strlen(sql), PDO_FROSTLAKE_COUNT_UNSET);
    if (response == NULL) {
        return false;
    }
    fl_json_free(response);
    return true;
}

static bool pdo_frostlake_handle_begin(pdo_dbh_t *dbh) {
    return run_transaction_statement(dbh, "BEGIN");
}

static bool pdo_frostlake_handle_commit(pdo_dbh_t *dbh) {
    return run_transaction_statement(dbh, "COMMIT");
}

static bool pdo_frostlake_handle_rollback(pdo_dbh_t *dbh) {
    return run_transaction_statement(dbh, "ROLLBACK");
}

static bool pdo_frostlake_set_attribute(pdo_dbh_t *dbh, zend_long attr, zval *val) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    switch (attr) {
        case PDO_ATTR_AUTOCOMMIT:
            /* Recorded and reported, but the session keeps the mode the connection was opened
             * with: pdo_snowflake sends autocommit once, when it logs in. */
            dbh->auto_commit = zval_get_long(val) ? 1 : 0;
            return true;
        case PDO_FROSTLAKE_ATTR_STMT_MULTI_STMT_COUNT:
            if (H == NULL) {
                return false;
            }
            H->multi_statement_count = zval_get_long(val);
            return true;
        default:
            return false;
    }
}

/* Snowflake has no last-insert id; like pdo_snowflake, the call answers false and raises nothing. */
static zend_string *pdo_frostlake_last_insert_id(pdo_dbh_t *dbh, const zend_string *name) {
    return NULL;
}

static void pdo_frostlake_fetch_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, zval *info) {
    const char *message = NULL;
    if (stmt != NULL) {
        pdo_frostlake_stmt *S = stmt->driver_data;
        message = S != NULL ? S->errmsg : NULL;
    } else {
        pdo_frostlake_db_handle *H = dbh->driver_data;
        message = H != NULL ? H->errmsg : NULL;
    }
    if (message != NULL) {
        /* The engine reports no native error code. */
        add_next_index_null(info);
        add_next_index_string(info, message);
    }
}

static int pdo_frostlake_get_attribute(pdo_dbh_t *dbh, zend_long attr, zval *return_value) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    switch (attr) {
        case PDO_ATTR_AUTOCOMMIT:
            ZVAL_LONG(return_value, dbh->auto_commit);
            return 1;
        case PDO_ATTR_CLIENT_VERSION:
            ZVAL_STRING(return_value, PHP_PDO_FROSTLAKE_VERSION);
            return 1;
        case PDO_FROSTLAKE_ATTR_STMT_MULTI_STMT_COUNT:
            if (H == NULL) {
                return 0;
            }
            ZVAL_LONG(return_value, H->multi_statement_count);
            return 1;
        default:
            return 0;
    }
}

/* A persistent connection is reused only while its session still exists: a server restart or the
 * idle timeout ends the session, and a reused handle would then fail every statement. */
static zend_result pdo_frostlake_check_liveness(pdo_dbh_t *dbh) {
    pdo_frostlake_db_handle *H = dbh->driver_data;
    if (H == NULL) {
        return FAILURE;
    }
    bool alive;
    if (H->session_id == NULL) {
        alive = pdo_frostlake_healthy(dbh);
    } else {
        fl_json *response = pdo_frostlake_execute(dbh, NULL, "SELECT 1", strlen("SELECT 1"), PDO_FROSTLAKE_COUNT_UNSET);
        alive = response != NULL;
        fl_json_free(response);
    }
    if (!alive && dbh->refcount <= 1) {
        /* PDO abandons the handle now, and some PHP 8.4 releases never close an abandoned
         * persistent handle, so let go of what the driver holds. Only when no PDO object still
         * holds the handle: one that does keeps using it, and its statements report the loss. */
        free_handle(dbh);
    }
    return alive ? SUCCESS : FAILURE;
}

static const struct pdo_dbh_methods pdo_frostlake_methods = {
    pdo_frostlake_handle_closer,
    pdo_frostlake_handle_preparer,
    pdo_frostlake_handle_doer,
    NULL, /* quoter: pdo_snowflake does not quote either, so PDO::quote() raises IM001 */
    pdo_frostlake_handle_begin,
    pdo_frostlake_handle_commit,
    pdo_frostlake_handle_rollback,
    pdo_frostlake_set_attribute,
    pdo_frostlake_last_insert_id,
    pdo_frostlake_fetch_error,
    pdo_frostlake_get_attribute,
    pdo_frostlake_check_liveness,
    NULL, /* get_driver_methods */
    NULL, /* persistent_shutdown */
    NULL, /* in_transaction: PDO's own tracking */
    NULL, /* get_gc */
#if PHP_VERSION_ID >= 80400
    NULL  /* scanner */
#endif
};

/* Runs one of the connection's USE statements. Answers false only when the connection has to be
 * refused: the server could not be reached, or `required` is set and the engine refused. */
static bool use_object(pdo_dbh_t *dbh, const char *kind, const char *name, bool required) {
    if (name == NULL || *name == '\0') {
        return true;
    }
    if (!pdo_frostlake_is_identifier(name)) {
        if (!required) {
            /* A name that could not name anything selects nothing, as a login leaves it. */
            return true;
        }
        char *message;
        spprintf(&message, 0, "The %s in the data source is not an identifier: %s", kind, name);
        pdo_frostlake_error(dbh, NULL, "08001", message);
        efree(message);
        return false;
    }

    char *sql;
    size_t sql_length = spprintf(&sql, 0, "USE %s %s", kind, name);
    /* While connecting a refusal would throw at once; decide first whether it is one. */
    const struct pdo_dbh_methods *connecting = dbh->methods;
    dbh->methods = &pdo_frostlake_methods;
    fl_json *response = pdo_frostlake_execute(dbh, NULL, sql, sql_length, PDO_FROSTLAKE_COUNT_UNSET);
    dbh->methods = connecting;
    efree(sql);
    if (response != NULL) {
        fl_json_free(response);
        return true;
    }

    bool refused_by_engine = strcmp(dbh->error_code, "HY000") == 0;
    if (refused_by_engine && !required) {
        /* A Snowflake login does not check the database, schema or warehouse it is given: one
         * that does not exist leaves the session without it. */
        strcpy(dbh->error_code, PDO_ERR_NONE);
        clear_error(dbh);
        return true;
    }
    pdo_frostlake_db_handle *H = dbh->driver_data;
    pdo_throw_exception(0, H->errmsg != NULL ? H->errmsg : (char *) "The connection was refused", &dbh->error_code);
    return false;
}

static int pdo_frostlake_handle_factory(pdo_dbh_t *dbh, zval *driver_options) {
    enum { HOST, PORT, DATABASE, SCHEMA, WAREHOUSE, ROLE, VAR_COUNT };
    struct pdo_data_src_parser vars[] = {
        {"host",      "localhost", 0},
        {"port",      "18082",     0},
        {"database",  NULL,        0},
        {"schema",    NULL,        0},
        {"warehouse", NULL,        0},
        {"role",      NULL,        0},
    };
    int ok = 0;

    /* Every other key pdo_snowflake reads — account, protocol, authenticator and the rest — means
     * nothing to a Frostlake server and is ignored, as are the user name and password. */
    php_pdo_parse_data_source(dbh->data_source, dbh->data_source_len, vars, VAR_COUNT);

    pdo_frostlake_db_handle *H = pecalloc(1, sizeof(pdo_frostlake_db_handle), dbh->is_persistent);
    dbh->driver_data = H;
    H->host = pestrdup(vars[HOST].optval, dbh->is_persistent);
    H->multi_statement_count = PDO_FROSTLAKE_COUNT_UNSET;
    H->opened_auto_commit = dbh->auto_commit;
    H->timeout = (int) pdo_attr_lval(driver_options, PDO_ATTR_TIMEOUT, PDO_FROSTLAKE_DEFAULT_TIMEOUT);
    if (H->timeout < 0) {
        H->timeout = PDO_FROSTLAKE_DEFAULT_TIMEOUT;
    }

    char *port_end = NULL;
    long port = strtol(vars[PORT].optval, &port_end, 10);
    if (port_end == vars[PORT].optval || *port_end != '\0' || port < 1 || port > 65535) {
        char *message;
        spprintf(&message, 0, "Invalid port in the data source: %s", vars[PORT].optval);
        pdo_frostlake_error(dbh, NULL, "08001", message);
        efree(message);
        goto cleanup;
    }
    H->port = (int) port;

    if (!pdo_frostlake_healthy(dbh)) {
        goto cleanup;
    }

    /* In the order a Snowflake login applies them: the role decides what the rest may use. */
    if (!use_object(dbh, "ROLE", vars[ROLE].optval, true)
            || !use_object(dbh, "WAREHOUSE", vars[WAREHOUSE].optval, false)
            || !use_object(dbh, "DATABASE", vars[DATABASE].optval, false)
            || !use_object(dbh, "SCHEMA", vars[SCHEMA].optval, false)) {
        goto cleanup;
    }

    dbh->alloc_own_columns = 1;
    dbh->methods = &pdo_frostlake_methods;
    ok = 1;

cleanup:
    for (int i = 0; i < VAR_COUNT; i++) {
        if (vars[i].freeme) {
            efree(vars[i].optval);
        }
    }
    if (!ok) {
        pdo_frostlake_release_session(H);
        free_handle(dbh);
    }
    return ok;
}

const pdo_driver_t pdo_frostlake_driver = {
    PDO_DRIVER_HEADER(frostlake),
    pdo_frostlake_handle_factory
};
