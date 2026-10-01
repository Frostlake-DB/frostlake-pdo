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
 * Rendering bound values into SQL.
 *
 * The engine's protocol has no server-side binding, so parameters are substituted here, the way
 * every other Frostlake driver does it. PDO's own placeholder parser is deliberately not used: it
 * does not know Snowflake SQL, and would read the VARIANT path access in `v:field` as a named
 * parameter `:field`, or a `?` inside a `$$`-quoted procedure body as a positional one.
 */

#include "php.h"
#include "zend_smart_str.h"

#include "php_pdo_frostlake_int.h"

#include <ctype.h>
#include <string.h>

/* If `c` opens a stretch of SQL in which a `?` or `:name` is text rather than a placeholder,
 * answer the position just past that stretch; otherwise NULL.
 *
 * The set matches the other Frostlake drivers: single-quoted strings (where a backslash escapes
 * the next character and '' is a doubled quote), quoted identifiers, -- // and slash-star
 * comments, and $$-delimited bodies. An unterminated construct swallows the rest of the text. */
static const char *skip_non_placeholder(const char *start, const char *c, const char *end) {
    if (*c == '\'') {
        const char *p = c + 1;
        while (p < end) {
            if (*p == '\\' && p + 1 < end) {
                p += 2;
            } else if (*p == '\'') {
                if (p + 1 >= end || p[1] != '\'') {
                    return p + 1;
                }
                p += 2;
            } else {
                p++;
            }
        }
        return end;
    }
    if (*c == '"') {
        const char *p = c + 1;
        while (p < end) {
            if (*p == '"') {
                if (p + 1 >= end || p[1] != '"') {
                    return p + 1;
                }
                p += 2;
            } else {
                p++;
            }
        }
        return end;
    }
    if (c + 1 < end && ((c[0] == '-' && c[1] == '-') || (c[0] == '/' && c[1] == '/'))) {
        const char *p = memchr(c + 2, '\n', (size_t) (end - (c + 2)));
        return p != NULL ? p + 1 : end;
    }
    if (c + 1 < end && c[0] == '/' && c[1] == '*') {
        for (const char *p = c + 2; p + 1 < end; p++) {
            if (p[0] == '*' && p[1] == '/') {
                return p + 2;
            }
        }
        return end;
    }
    /* `$$` inside an identifier such as a$$b opens nothing. */
    if (c + 1 < end && c[0] == '$' && c[1] == '$'
            && (c == start || !(isalnum((unsigned char) c[-1]) || c[-1] == '_' || c[-1] == '$'))) {
        for (const char *p = c + 2; p + 1 < end; p++) {
            if (p[0] == '$' && p[1] == '$') {
                return p + 2;
            }
        }
        return end;
    }
    return NULL;
}

/* Whether the `:` at `c` opens a named parameter.
 *
 * A name must follow, so `::` (a cast), `:=` (an assignment) and `:1` (a positional reference) are
 * never parameters. Nor is a colon straight after the end of an operand — a word character, a
 * closing parenthesis, bracket, brace or quote, or a `?` — which reads a field of a semi-structured
 * value: `v:field`, `PARSE_JSON('{}'):k`, `arr[0]:k`, `{'k': 1}:k`, `"V":k`, `?:k`. With a blank
 * before it, the colon opens a parameter again. */
static int opens_named_parameter(const char *start, const char *c, const char *end) {
    if (c + 1 >= end || !(isalpha((unsigned char) c[1]) || c[1] == '_')) {
        return 0;
    }
    if (c > start) {
        unsigned char prev = (unsigned char) c[-1];
        if (isalnum(prev) || prev == '_' || prev == '$' || prev == ')' || prev == ']' || prev == '}'
                || prev == '\'' || prev == '"' || prev == '?' || prev == ':') {
            return 0;
        }
    }
    return 1;
}

/* Whether text is exactly a SQL numeric literal: an optional sign, digits with at most one point,
 * and an optional exponent. Blanks around it are tolerated. */
static int is_numeric_literal(const char *text, size_t length) {
    size_t i = 0;
    size_t end = length;
    while (i < end && (text[i] == ' ' || text[i] == '\t')) {
        i++;
    }
    while (end > i && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        end--;
    }
    if (i == end) {
        return 0;
    }
    if (text[i] == '+' || text[i] == '-') {
        i++;
    }
    int digits = 0;
    while (i < end && isdigit((unsigned char) text[i])) {
        i++;
        digits++;
    }
    if (i < end && text[i] == '.') {
        i++;
        while (i < end && isdigit((unsigned char) text[i])) {
            i++;
            digits++;
        }
    }
    if (digits == 0) {
        return 0;
    }
    if (i < end && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < end && (text[i] == '+' || text[i] == '-')) {
            i++;
        }
        int exponent_digits = 0;
        while (i < end && isdigit((unsigned char) text[i])) {
            i++;
            exponent_digits++;
        }
        if (exponent_digits == 0) {
            return 0;
        }
    }
    return i == end;
}

/* A string as a quoted literal. A backslash is an escape character in Snowflake string literals,
 * so it is doubled along with the quote. */
static int append_string_literal(fl_strbuf *buf, const char *text, size_t length) {
    if (fl_strbuf_append(buf, "'") != 0) {
        return -1;
    }
    size_t run = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\'' || text[i] == '\\') {
            /* Flush the plain run with this character, then repeat the character. */
            if (fl_strbuf_append_len(buf, text + run, i - run + 1) != 0) {
                return -1;
            }
            run = i;
        }
    }
    if (fl_strbuf_append_len(buf, text + run, length - run) != 0) {
        return -1;
    }
    return fl_strbuf_append(buf, "'");
}

static int append_binary_literal(fl_strbuf *buf, const unsigned char *bytes, size_t length) {
    static const char hex[] = "0123456789ABCDEF";
    if (fl_strbuf_append(buf, "X'") != 0) {
        return -1;
    }
    for (size_t i = 0; i < length; i++) {
        char pair[2] = { hex[bytes[i] >> 4], hex[bytes[i] & 0x0F] };
        if (fl_strbuf_append_len(buf, pair, 2) != 0) {
            return -1;
        }
    }
    return fl_strbuf_append(buf, "'");
}

int pdo_frostlake_is_identifier(const char *text) {
    size_t length = strlen(text);
    if (length == 0) {
        return 0;
    }
    if (text[0] == '"') {
        if (length < 3 || text[length - 1] != '"') {
            return 0;
        }
        for (size_t i = 1; i < length - 1; i++) {
            if (text[i] == '"') {
                if (i + 1 >= length - 1 || text[i + 1] != '"') {
                    return 0;
                }
                i++;
            }
        }
        return 1;
    }
    if (!(isalpha((unsigned char) text[0]) || text[0] == '_')) {
        return 0;
    }
    for (size_t i = 1; i < length; i++) {
        if (!(isalnum((unsigned char) text[i]) || text[i] == '_' || text[i] == '$')) {
            return 0;
        }
    }
    return 1;
}

int pdo_frostlake_append_json(fl_strbuf *buf, const fl_json *node) {
    if (node == NULL) {
        return fl_strbuf_append(buf, "null");
    }
    switch (node->kind) {
        case FL_JSON_NULL:
            return fl_strbuf_append(buf, "null");
        case FL_JSON_BOOL:
        case FL_JSON_NUMBER:
            return fl_strbuf_append(buf, node->text);
        case FL_JSON_STRING:
            return fl_strbuf_append_json_string(buf, node->text);
        case FL_JSON_ARRAY:
        case FL_JSON_OBJECT: {
            int object = node->kind == FL_JSON_OBJECT;
            if (fl_strbuf_append(buf, object ? "{" : "[") != 0) {
                return -1;
            }
            for (size_t i = 0; i < node->child_count; i++) {
                if (i > 0 && fl_strbuf_append(buf, ",") != 0) {
                    return -1;
                }
                if (object && (fl_strbuf_append_json_string(buf, node->keys[i]) != 0
                               || fl_strbuf_append(buf, ":") != 0)) {
                    return -1;
                }
                if (pdo_frostlake_append_json(buf, node->children[i]) != 0) {
                    return -1;
                }
            }
            return fl_strbuf_append(buf, object ? "}" : "]");
        }
    }
    return -1;
}

/* A number as a bare literal. After a `-` in the statement, a negative number would open a line
 * comment — `10 -?` bound to -5 must read `10 - -5`, not `10 --5` — so a space goes between. */
static int append_number(fl_strbuf *buf, const char *text, size_t length) {
    if (length > 0 && text[0] == '-' && buf->length > 0 && buf->data[buf->length - 1] == '-'
            && fl_strbuf_append(buf, " ") != 0) {
        return -1;
    }
    return fl_strbuf_append_len(buf, text, length);
}

static int append_double(fl_strbuf *buf, double d) {
    if (zend_isnan(d)) {
        return fl_strbuf_append(buf, "'NaN'::FLOAT");
    }
    if (zend_isinf(d)) {
        return fl_strbuf_append(buf, d > 0 ? "'inf'::FLOAT" : "'-inf'::FLOAT");
    }
    smart_str text = {0};
    smart_str_append_double(&text, d, (int) PG(serialize_precision), false);
    smart_str_0(&text);
    int rc = append_number(buf, ZSTR_VAL(text.s), ZSTR_LEN(text.s));
    smart_str_free(&text);
    return rc;
}

static int append_as_string(fl_strbuf *buf, zval *value, const char **sqlstate, char **message) {
    zend_string *text = zval_try_get_string(value);
    if (text == NULL) {
        *sqlstate = "HY105";
        *message = estrdup("A parameter could not be converted to a string");
        return -1;
    }
    int rc = append_string_literal(buf, ZSTR_VAL(text), ZSTR_LEN(text));
    zend_string_release(text);
    return rc;
}

/* One bound value as a SQL literal of the type the binding declared, the way pdo_snowflake hands
 * each type to the server: a string binding stays a string whatever PHP value it holds. */
static int append_value(fl_strbuf *buf, struct pdo_bound_param_data *param,
                        const char **sqlstate, char **message) {
    zval *value = &param->parameter;
    ZVAL_DEREF(value);

    if (Z_TYPE_P(value) == IS_NULL) {
        return fl_strbuf_append(buf, "NULL");
    }

    switch (PDO_PARAM_TYPE(param->param_type)) {
        case PDO_PARAM_NULL:
            return fl_strbuf_append(buf, "NULL");

        case PDO_PARAM_BOOL:
            return fl_strbuf_append(buf, zend_is_true(value) ? "TRUE" : "FALSE");

        case PDO_PARAM_INT:
            switch (Z_TYPE_P(value)) {
                case IS_LONG: {
                    char text[MAX_LENGTH_OF_LONG + 1];
                    int length = snprintf(text, sizeof(text), ZEND_LONG_FMT, Z_LVAL_P(value));
                    return append_number(buf, text, (size_t) length);
                }
                case IS_TRUE:
                    return fl_strbuf_append(buf, "1");
                case IS_FALSE:
                    return fl_strbuf_append(buf, "0");
                case IS_DOUBLE:
                    return append_double(buf, Z_DVAL_P(value));
                case IS_STRING:
                    /* Only an actual numeric literal lands unquoted; anything else stays a
                     * string, so "0 OR 1=1" can never become SQL. */
                    if (is_numeric_literal(Z_STRVAL_P(value), Z_STRLEN_P(value))) {
                        return append_number(buf, Z_STRVAL_P(value), Z_STRLEN_P(value));
                    }
                    return append_string_literal(buf, Z_STRVAL_P(value), Z_STRLEN_P(value));
                default:
                    return append_as_string(buf, value, sqlstate, message);
            }

        case PDO_PARAM_LOB: {
            /* Binary data: a string's bytes, or everything a stream has left to give. */
            zend_string *bytes;
            if (Z_TYPE_P(value) == IS_RESOURCE) {
                php_stream *stream = NULL;
                php_stream_from_zval_no_verify(stream, value);
                if (stream == NULL) {
                    *sqlstate = "HY105";
                    *message = estrdup("Expected a stream resource for a PDO::PARAM_LOB parameter");
                    return -1;
                }
                bytes = php_stream_copy_to_mem(stream, PHP_STREAM_COPY_ALL, 0);
                if (bytes == NULL) {
                    bytes = ZSTR_EMPTY_ALLOC();
                }
            } else {
                bytes = zval_try_get_string(value);
                if (bytes == NULL) {
                    *sqlstate = "HY105";
                    *message = estrdup("A PDO::PARAM_LOB parameter could not be read as bytes");
                    return -1;
                }
            }
            int rc = append_binary_literal(buf, (const unsigned char *) ZSTR_VAL(bytes), ZSTR_LEN(bytes));
            zend_string_release(bytes);
            return rc;
        }

        default:
            return append_as_string(buf, value, sqlstate, message);
    }
}

/* A refusal's *message is the detail alone. PDO writes the SQLSTATE and its description in front of
 * it: "SQLSTATE[HY093]: Invalid parameter number: parameter was not defined: :id". */
zend_string *pdo_frostlake_render_statement(pdo_stmt_t *stmt, const char **sqlstate, char **message) {
    /* With nothing bound, every marker is the engine's to read — a Snowflake Scripting cursor
     * opened USING (...) binds its own `?` — so the text goes through untouched. */
    if (stmt->bound_params == NULL || zend_hash_num_elements(stmt->bound_params) == 0) {
        return zend_string_copy(stmt->query_string);
    }

    const char *start = ZSTR_VAL(stmt->query_string);
    const char *end = start + ZSTR_LEN(stmt->query_string);
    const char *c = start;
    fl_strbuf out;
    fl_strbuf_init(&out);

    zend_long position = 0;
    int positional = 0;
    int named = 0;
    HashTable used_names;
    zend_hash_init(&used_names, 8, NULL, NULL, 0);

    while (c < end) {
        const char *past = skip_non_placeholder(start, c, end);
        if (past != NULL) {
            if (fl_strbuf_append_len(&out, c, (size_t) (past - c)) != 0) {
                goto oom;
            }
            c = past;
            continue;
        }
        if (*c == '?') {
            positional = 1;
            struct pdo_bound_param_data *param =
                zend_hash_index_find_ptr(stmt->bound_params, (zend_ulong) position);
            if (param == NULL) {
                *sqlstate = "HY093";
                *message = estrdup("number of bound variables does not match number of tokens");
                goto fail;
            }
            if (append_value(&out, param, sqlstate, message) != 0) {
                goto fail;
            }
            position++;
            c++;
            continue;
        }
        if (*c == ':' && opens_named_parameter(start, c, end)) {
            const char *name_end = c + 1;
            while (name_end < end && (isalnum((unsigned char) *name_end) || *name_end == '_')) {
                name_end++;
            }
            named = 1;
            struct pdo_bound_param_data *param =
                zend_hash_str_find_ptr(stmt->bound_params, c, (size_t) (name_end - c));
            if (param == NULL) {
                *sqlstate = "HY093";
                smart_str text = {0};
                smart_str_appends(&text, "parameter was not defined: ");
                smart_str_appendl(&text, c, (size_t) (name_end - c));
                smart_str_0(&text);
                *message = estrndup(ZSTR_VAL(text.s), ZSTR_LEN(text.s));
                smart_str_free(&text);
                goto fail;
            }
            zend_hash_str_add_empty_element(&used_names, c, (size_t) (name_end - c));
            if (append_value(&out, param, sqlstate, message) != 0) {
                goto fail;
            }
            c = name_end;
            continue;
        }
        if (fl_strbuf_append_len(&out, c, 1) != 0) {
            goto oom;
        }
        c++;
    }

    if (positional && named) {
        *sqlstate = "HY093";
        *message = estrdup("mixed named and positional parameters");
        goto fail;
    }
    uint32_t bound = zend_hash_num_elements(stmt->bound_params);
    uint32_t consumed = positional ? (uint32_t) position : zend_hash_num_elements(&used_names);
    if (consumed != bound) {
        *sqlstate = "HY093";
        *message = estrdup("number of bound variables does not match number of tokens");
        goto fail;
    }

    zend_hash_destroy(&used_names);
    zend_string *result = zend_string_init(out.data != NULL ? out.data : "", out.length, 0);
    fl_strbuf_free(&out);
    return result;

oom:
    *sqlstate = "HY001";
    *message = estrdup("Out of memory rendering the statement");
fail:
    zend_hash_destroy(&used_names);
    fl_strbuf_free(&out);
    return NULL;
}

/* ---------------------------------------------------------------- session tracking
 *
 * What a statement leaves on the session, read with the same scanner as the placeholders, so that
 * the two cannot disagree about what is code and what is quoted. */

#define SESSION_WORDS 16
#define SESSION_WORD_MAX 32

/* A byte of an unquoted identifier or keyword: `$` is one, which is why A$$B is a name, and so is
 * every byte of a multibyte letter. */
static int is_word_byte(unsigned char c) {
    return isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

/* Up to SESSION_WORDS leading words of the statement [c, end), upper-cased, skipping blanks and
 * comments and stopping at the first thing that is not a word. A word too long for the buffer is
 * cut short, which no keyword is. */
static int leading_words(const char *c, const char *end, char words[SESSION_WORDS][SESSION_WORD_MAX]) {
    int count = 0;
    while (count < SESSION_WORDS && c < end) {
        if (isspace((unsigned char) *c)) {
            c++;
        } else if (c + 1 < end && ((c[0] == '-' && c[1] == '-') || (c[0] == '/' && c[1] == '/'))) {
            const char *line_end = memchr(c + 2, '\n', (size_t) (end - (c + 2)));
            c = line_end != NULL ? line_end + 1 : end;
        } else if (c + 1 < end && c[0] == '/' && c[1] == '*') {
            const char *p = c + 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) {
                p++;
            }
            c = p + 1 < end ? p + 2 : end;
        } else if (is_word_byte((unsigned char) *c)) {
            size_t length = 0;
            while (c < end && is_word_byte((unsigned char) *c)) {
                if (length + 1 < SESSION_WORD_MAX) {
                    words[count][length++] = (char) toupper((unsigned char) *c);
                }
                c++;
            }
            words[count][length] = '\0';
            count++;
        } else {
            break;
        }
    }
    return count;
}

static int is_one_of(const char *word, const char *const *set) {
    for (; *set != NULL; set++) {
        if (strcmp(word, *set) == 0) {
            return 1;
        }
    }
    return 0;
}

/* The words that may sit between CREATE, DROP or ALTER and the kind of object being named. */
static const char *const object_modifiers[] = {
    "OR", "REPLACE", "TRANSIENT", "TEMPORARY", "TEMP", "VOLATILE", "LOCAL", "GLOBAL", "SECURE", "IF",
    "NOT", "EXISTS", "PUBLIC", "PRIVATE", "ICEBERG", "DYNAMIC", "HYBRID", "EVENT", "RECURSIVE",
    "MATERIALIZED", "EXTERNAL", NULL
};

static const char *const temporary_modifiers[] = {"TEMPORARY", "TEMP", "VOLATILE", NULL};

/* Whether a statement leaves behind state a fresh session would not have: a moved scope (USE, or
 * CREATE or DROP of a DATABASE or SCHEMA), a session variable or setting (SET, UNSET, ALTER
 * SESSION), or a temporary object. CREATE TABLE and its kind leave the session as it was. */
static int touches_session(char words[SESSION_WORDS][SESSION_WORD_MAX], int count) {
    if (count == 0) {
        return 0;
    }
    const char *verb = words[0];
    if (strcmp(verb, "USE") == 0 || strcmp(verb, "SET") == 0 || strcmp(verb, "UNSET") == 0) {
        return 1;
    }
    int alter = strcmp(verb, "ALTER") == 0;
    int create = strcmp(verb, "CREATE") == 0;
    if (!alter && !create && strcmp(verb, "DROP") != 0) {
        return 0;
    }
    int i = 1;
    int temporary = 0;
    while (i < count && is_one_of(words[i], object_modifiers)) {
        temporary = temporary || is_one_of(words[i], temporary_modifiers);
        i++;
    }
    const char *kind = i < count ? words[i] : "";
    if (alter) {
        return strcmp(kind, "SESSION") == 0;
    }
    return strcmp(kind, "DATABASE") == 0 || strcmp(kind, "SCHEMA") == 0 || (create && temporary);
}

/* 1 when a statement opens a transaction, -1 when it ends one, 0 otherwise. BEGIN on its own (or
 * with TRANSACTION, WORK or NAME) opens one; BEGIN followed by a statement opens a scripting block
 * instead. */
static int transaction_effect(char words[SESSION_WORDS][SESSION_WORD_MAX], int count) {
    if (count == 0) {
        return 0;
    }
    const char *second = count > 1 ? words[1] : NULL;
    if (strcmp(words[0], "COMMIT") == 0 || strcmp(words[0], "ROLLBACK") == 0) {
        return -1;
    }
    if (strcmp(words[0], "START") == 0) {
        return second != NULL && strcmp(second, "TRANSACTION") == 0 ? 1 : 0;
    }
    if (strcmp(words[0], "BEGIN") != 0) {
        return 0;
    }
    return second == NULL || strcmp(second, "TRANSACTION") == 0 || strcmp(second, "WORK") == 0
        || strcmp(second, "NAME") == 0 ? 1 : 0;
}

/* The request is read statement by statement, split on its top-level semicolons. A scripting block
 * is split along with everything else, which only makes the checks more willing to flag a request —
 * the safe direction to be wrong in. */
void pdo_frostlake_track_session(pdo_frostlake_db_handle *H, const char *sql, size_t length) {
    const char *end = sql + length;
    const char *statement = sql;
    const char *c = sql;
    for (;;) {
        if (c < end) {
            const char *past = skip_non_placeholder(sql, c, end);
            if (past != NULL) {
                c = past;
                continue;
            }
            if (*c != ';') {
                c++;
                continue;
            }
        }
        char words[SESSION_WORDS][SESSION_WORD_MAX];
        int count = leading_words(statement, c, words);
        if (touches_session(words, count)) {
            H->dirty = true;
        }
        int effect = transaction_effect(words, count);
        if (effect < 0) {
            H->in_transaction = false;
        } else if (effect > 0 || (!H->opened_auto_commit && count > 0)) {
            /* With autocommit off, any statement may have opened a transaction of its own. */
            H->in_transaction = true;
        }
        if (c >= end) {
            break;
        }
        c++;
        statement = c;
    }
}
