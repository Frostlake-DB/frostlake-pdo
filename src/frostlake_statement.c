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
 * Statements: executing, walking result sets and rows, and describing columns.
 *
 * Every value reaches PHP as a string, or null, as pdo_snowflake delivers it; the string is the
 * engine's exact text, so a NUMBER keeps every digit.
 */

#include "php.h"

#include "php_pdo_frostlake_int.h"

#include <string.h>

/* The widths libsnowflakeclient assumes for a column whose result metadata gives none. */
#define DEFAULT_MAX_TEXT_SIZE 16777216
#define DEFAULT_MAX_BINARY_SIZE (DEFAULT_MAX_TEXT_SIZE / 2)
#define DEFAULT_MAX_VARIANT_SIZE 16777216
/* The largest value a text or binary column holds, in bytes. */
#define MAX_VALUE_BYTES 134217728

zend_long pdo_frostlake_result_row_count(const fl_json *result_set) {
    long update_count = fl_json_get_long(result_set, "updateCount", -1);
    if (update_count >= 0) {
        return (zend_long) update_count;
    }
    const fl_json *rows = fl_json_get(result_set, "rows");
    return rows != NULL ? (zend_long) rows->child_count : 0;
}

/* The engine's type name, without any (p,s) or (n) that follows it, compared case-insensitively. */
static bool type_is(const char *data_type, const char *name) {
    size_t length = strlen(name);
    return strncasecmp(data_type, name, length) == 0
        && (data_type[length] == '\0' || data_type[length] == '(' || data_type[length] == ' ');
}

/* The type as Snowflake's result metadata names it, which is what pdo_snowflake reports as
 * native_type; a type it does not know reads TEXT there too. */
static const char *snowflake_type_name(const char *data_type) {
    static const char *const fixed[] = {
        "NUMBER", "DECIMAL", "NUMERIC", "INT", "INTEGER", "BIGINT", "SMALLINT", "TINYINT", "BYTEINT", NULL
    };
    static const char *const real[] = {
        "FLOAT", "FLOAT4", "FLOAT8", "DOUBLE", "REAL", NULL
    };
    static const char *const binary[] = {"BINARY", "VARBINARY", NULL};
    static const char *const same[] = {
        "BOOLEAN", "DATE", "TIME", "TIMESTAMP_NTZ", "TIMESTAMP_LTZ", "TIMESTAMP_TZ",
        "VARIANT", "OBJECT", "ARRAY", "DECFLOAT", NULL
    };
    if (data_type == NULL) {
        return "TEXT";
    }
    for (int i = 0; fixed[i] != NULL; i++) {
        if (type_is(data_type, fixed[i])) {
            return "FIXED";
        }
    }
    for (int i = 0; real[i] != NULL; i++) {
        if (type_is(data_type, real[i])) {
            return "REAL";
        }
    }
    for (int i = 0; binary[i] != NULL; i++) {
        if (type_is(data_type, binary[i])) {
            return "BINARY";
        }
    }
    for (int i = 0; same[i] != NULL; i++) {
        if (type_is(data_type, same[i])) {
            return same[i];
        }
    }
    if (type_is(data_type, "DATETIME") || type_is(data_type, "TIMESTAMP")) {
        return "TIMESTAMP_NTZ";
    }
    return "TEXT";
}

/* The column's size in bytes, as pdo_snowflake reports it for the column's `len`. */
static size_t column_byte_size(const fl_json *column) {
    const char *type = snowflake_type_name(fl_json_get_string(column, "dataType"));
    long length = fl_json_get_long(column, "length", -1);
    if (strcmp(type, "TEXT") == 0) {
        /* four bytes to a character, as Snowflake budgets a string */
        if (length < 0) {
            return DEFAULT_MAX_TEXT_SIZE;
        }
        return length > MAX_VALUE_BYTES / 4 ? MAX_VALUE_BYTES : (size_t) length * 4;
    }
    if (strcmp(type, "BINARY") == 0) {
        return length < 0 ? DEFAULT_MAX_BINARY_SIZE : (size_t) length;
    }
    if (strcmp(type, "VARIANT") == 0 || strcmp(type, "OBJECT") == 0 || strcmp(type, "ARRAY") == 0) {
        return DEFAULT_MAX_VARIANT_SIZE;
    }
    if (strcmp(type, "BOOLEAN") == 0) {
        /* the longer of the two texts a boolean reads as */
        return 1;
    }
    return 0;
}

static void clear_result(pdo_frostlake_stmt *S) {
    if (S->response != NULL) {
        fl_json_free(S->response);
        S->response = NULL;
    }
    S->result_set = 0;
    S->columns = NULL;
    S->rows = NULL;
    S->row = -1;
}

static void enter_result_set(pdo_stmt_t *stmt, const fl_json *result_set) {
    pdo_frostlake_stmt *S = stmt->driver_data;
    S->columns = fl_json_get(result_set, "columns");
    S->rows = fl_json_get(result_set, "rows");
    S->row = -1;
    php_pdo_stmt_set_column_count(stmt, S->columns != NULL ? (int) S->columns->child_count : 0);
    stmt->row_count = pdo_frostlake_result_row_count(result_set);
}

static int pdo_frostlake_stmt_dtor(pdo_stmt_t *stmt) {
    pdo_frostlake_stmt *S = stmt->driver_data;
    if (S != NULL) {
        clear_result(S);
        if (S->errmsg != NULL) {
            efree(S->errmsg);
        }
        efree(S);
        stmt->driver_data = NULL;
    }
    return 1;
}

static int pdo_frostlake_stmt_execute(pdo_stmt_t *stmt) {
    pdo_frostlake_stmt *S = stmt->driver_data;
    if (S->errmsg != NULL) {
        efree(S->errmsg);
        S->errmsg = NULL;
    }
    clear_result(S);

    const char *sqlstate = NULL;
    char *message = NULL;
    zend_string *sql = pdo_frostlake_render_statement(stmt, &sqlstate, &message);
    if (sql == NULL) {
        pdo_frostlake_error(stmt->dbh, stmt, sqlstate, message);
        efree(message);
        return 0;
    }
    /* What was sent, for PDOStatement::debugDumpParams(). */
    if (stmt->active_query_string != NULL) {
        zend_string_release(stmt->active_query_string);
    }
    stmt->active_query_string = sql;

    fl_json *response = pdo_frostlake_execute(stmt->dbh, stmt, ZSTR_VAL(sql), ZSTR_LEN(sql),
                                              S->multi_statement_count);
    if (response == NULL) {
        return 0;
    }
    S->response = response;

    const fl_json *first = fl_json_at(fl_json_get(response, "resultSets"), 0);
    if (first != NULL) {
        enter_result_set(stmt, first);
    } else {
        php_pdo_stmt_set_column_count(stmt, 0);
        stmt->row_count = 0;
    }
    return 1;
}

/* Rows come forward only, whatever orientation is asked for, as with pdo_snowflake. */
static int pdo_frostlake_stmt_fetch(pdo_stmt_t *stmt, enum pdo_fetch_orientation ori, zend_long offset) {
    pdo_frostlake_stmt *S = stmt->driver_data;
    if (S->rows == NULL || S->row + 1 >= (zend_long) S->rows->child_count) {
        return 0;
    }
    S->row++;
    return 1;
}

static int pdo_frostlake_stmt_describe(pdo_stmt_t *stmt, int colno) {
    pdo_frostlake_stmt *S = stmt->driver_data;
    const fl_json *column = fl_json_at(S->columns, (size_t) colno);
    if (column == NULL) {
        return 0;
    }
    struct pdo_column_data *col = &stmt->columns[colno];
    const char *name = fl_json_get_string(column, "name");
    col->name = zend_string_init(name != NULL ? name : "", name != NULL ? strlen(name) : 0, 0);
    long precision = fl_json_get_long(column, "precision", 0);
    col->precision = precision > 0 ? (zend_ulong) precision : 0;
    col->maxlen = column_byte_size(column);
    return 1;
}

static int pdo_frostlake_stmt_get_col(pdo_stmt_t *stmt, int colno, zval *result, enum pdo_param_type *type) {
    pdo_frostlake_stmt *S = stmt->driver_data;
    const fl_json *cell = fl_json_at(fl_json_at(S->rows, (size_t) S->row), (size_t) colno);
    if (cell == NULL || cell->kind == FL_JSON_NULL) {
        ZVAL_NULL(result);
        return 1;
    }
    switch (cell->kind) {
        case FL_JSON_BOOL: {
            const fl_json *column = fl_json_at(S->columns, (size_t) colno);
            if (strcmp(snowflake_type_name(fl_json_get_string(column, "dataType")), "BOOLEAN") == 0) {
                /* libsnowflakeclient's text for a BOOLEAN: "1", and the empty string for false */
                if (strcmp(cell->text, "true") == 0) {
                    ZVAL_CHAR(result, '1');
                } else {
                    ZVAL_EMPTY_STRING(result);
                }
            } else {
                /* a boolean inside a column declared otherwise reads as its word */
                ZVAL_STRING(result, cell->text);
            }
            return 1;
        }
        case FL_JSON_ARRAY:
        case FL_JSON_OBJECT: {
            fl_strbuf text;
            fl_strbuf_init(&text);
            if (pdo_frostlake_append_json(&text, cell) != 0) {
                fl_strbuf_free(&text);
                pdo_frostlake_error(stmt->dbh, stmt, "HY001", "Out of memory reading a value");
                return 0;
            }
            ZVAL_STRINGL(result, text.data != NULL ? text.data : "", text.length);
            fl_strbuf_free(&text);
            return 1;
        }
        default:
            ZVAL_STRING(result, cell->text != NULL ? cell->text : "");
            return 1;
    }
}

static int pdo_frostlake_stmt_get_attribute(pdo_stmt_t *stmt, zend_long attr, zval *return_value) {
    return 0;
}

static int pdo_frostlake_stmt_column_meta(pdo_stmt_t *stmt, zend_long colno, zval *return_value) {
    pdo_frostlake_stmt *S = stmt->driver_data;
    const fl_json *column = colno >= 0 ? fl_json_at(S->columns, (size_t) colno) : NULL;
    if (column == NULL) {
        /* Refused here: a PDO older than 8.4's later releases reads the column's name without
         * checking the index itself. */
        pdo_frostlake_error(stmt->dbh, stmt, "07009", "Invalid column index");
        return FAILURE;
    }

    zval flags;
    array_init(return_value);
    array_init(&flags);
    if (!fl_json_get_bool(column, "nullable", 1)) {
        add_next_index_string(&flags, "not_null");
    }
    add_assoc_long(return_value, "scale", (zend_long) fl_json_get_long(column, "scale", 0));
    add_assoc_string(return_value, "native_type",
                     (char *) snowflake_type_name(fl_json_get_string(column, "dataType")));
    add_assoc_zval(return_value, "flags", &flags);
    return SUCCESS;
}

static int pdo_frostlake_stmt_next_rowset(pdo_stmt_t *stmt) {
    pdo_frostlake_stmt *S = stmt->driver_data;
    const fl_json *sets = fl_json_get(S->response, "resultSets");
    if (sets == NULL || S->result_set + 1 >= sets->child_count) {
        return 0;
    }
    S->result_set++;
    enter_result_set(stmt, fl_json_at(sets, S->result_set));
    return 1;
}

/* The whole answer arrived with the execution, so closing the cursor only lets it go. */
static int pdo_frostlake_stmt_cursor_closer(pdo_stmt_t *stmt) {
    clear_result(stmt->driver_data);
    return 1;
}

const struct pdo_stmt_methods pdo_frostlake_stmt_methods = {
    pdo_frostlake_stmt_dtor,
    pdo_frostlake_stmt_execute,
    pdo_frostlake_stmt_fetch,
    pdo_frostlake_stmt_describe,
    pdo_frostlake_stmt_get_col,
    NULL, /* param_hook: parameters are read at execution */
    NULL, /* set_attribute: none, as with pdo_snowflake */
    pdo_frostlake_stmt_get_attribute,
    pdo_frostlake_stmt_column_meta,
    pdo_frostlake_stmt_next_rowset,
    pdo_frostlake_stmt_cursor_closer
};
