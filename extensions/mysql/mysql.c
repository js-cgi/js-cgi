#include "../../js-cgi-module.h"
#include <mysql/mysql.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_CONNS 16

static MYSQL *g_conns[MAX_CONNS];
static int64_t g_last_affected[MAX_CONNS];
static int64_t g_last_insert_id[MAX_CONNS];
static int g_conn_count = 0;

/* -------------------- Helpers -------------------- */

static int find_conn_slot(void) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (!g_conns[i]) return i;
    }
    return -1;
}

static int get_conn_id(JSContext *ctx, JSValueConst this_val) {
    JSValue id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int id = JS_VALUE_GET_INT(id_val);
    JS_FreeValue(ctx, id_val);
    return id;
}

static MYSQL *get_conn(JSContext *ctx, JSValueConst this_val) {
    int id = get_conn_id(ctx, this_val);
    if (id < 0 || id >= MAX_CONNS || !g_conns[id]) return NULL;
    return g_conns[id];
}

static MYSQL_BIND *bind_params(JSContext *ctx, JSValue params, int *count,
                               unsigned long **lengths, char **null_flags) {
    if (JS_IsUndefined(params) || JS_IsNull(params)) {
        *count = 0;
        return NULL;
    }

    JSValue length_val = JS_GetPropertyStr(ctx, params, "length");
    int64_t len = 0;
    JS_ToInt64(ctx, &len, length_val);
    JS_FreeValue(ctx, length_val);

    if (len <= 0) {
        *count = 0;
        return NULL;
    }

    *count = (int)len;
    MYSQL_BIND *binds = calloc(len, sizeof(MYSQL_BIND));
    *lengths = calloc(len, sizeof(unsigned long));
    *null_flags = calloc(len, sizeof(char));

    for (int i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, params, i);
        int tag = JS_VALUE_GET_NORM_TAG(val);

        switch (tag) {
            case JS_TAG_INT: {
                int64_t *num = malloc(sizeof(int64_t));
                int32_t ival = JS_VALUE_GET_INT(val);
                *num = ival;
                binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
                binds[i].buffer = num;
                binds[i].is_null = (bool *)&(*null_flags)[i];
                (*null_flags)[i] = 0;
                break;
            }
            case JS_TAG_FLOAT64: {
                double *num = malloc(sizeof(double));
                *num = JS_VALUE_GET_FLOAT64(val);
                binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
                binds[i].buffer = num;
                binds[i].is_null = (bool *)&(*null_flags)[i];
                (*null_flags)[i] = 0;
                break;
            }
            case JS_TAG_STRING: {
                size_t slen;
                const char *str = JS_ToCStringLen(ctx, &slen, val);
                char *copy = malloc(slen + 1);
                memcpy(copy, str, slen);
                copy[slen] = '\0';
                JS_FreeCString(ctx, str);
                binds[i].buffer_type = MYSQL_TYPE_STRING;
                binds[i].buffer = copy;
                binds[i].buffer_length = slen;
                (*lengths)[i] = slen;
                binds[i].length = &(*lengths)[i];
                binds[i].is_null = (bool *)&(*null_flags)[i];
                (*null_flags)[i] = 0;
                break;
            }
            case JS_TAG_NULL:
            case JS_TAG_UNDEFINED:
            default:
                binds[i].buffer_type = MYSQL_TYPE_NULL;
                (*null_flags)[i] = 1;
                binds[i].is_null = (bool *)&(*null_flags)[i];
                break;
        }

        JS_FreeValue(ctx, val);
    }

    return binds;
}

static void free_binds(MYSQL_BIND *binds, int count, unsigned long *lengths, char *null_flags) {
    if (!binds) return;
    for (int i = 0; i < count; i++) {
        if (binds[i].buffer && binds[i].buffer_type != MYSQL_TYPE_NULL) {
            free(binds[i].buffer);
        }
    }
    free(binds);
    free(lengths);
    free(null_flags);
}

static JSValue result_to_array(JSContext *ctx, MYSQL_RES *res) {
    JSValue results = JS_NewArray(ctx);
    uint32_t idx = 0;

    int num_fields = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    MYSQL_ROW row;

    while ((row = mysql_fetch_row(res))) {
        unsigned long *row_lengths = mysql_fetch_lengths(res);
        JSValue obj = JS_NewObject(ctx);

        for (int i = 0; i < num_fields; i++) {
            if (row[i] == NULL) {
                JS_SetPropertyStr(ctx, obj, fields[i].name, JS_NULL);
            } else {
                switch (fields[i].type) {
                    case MYSQL_TYPE_TINY:
                    case MYSQL_TYPE_SHORT:
                    case MYSQL_TYPE_LONG:
                    case MYSQL_TYPE_INT24:
                        JS_SetPropertyStr(ctx, obj, fields[i].name,
                            JS_NewInt64(ctx, atoll(row[i])));
                        break;
                    case MYSQL_TYPE_LONGLONG:
                        JS_SetPropertyStr(ctx, obj, fields[i].name,
                            JS_NewInt64(ctx, atoll(row[i])));
                        break;
                    case MYSQL_TYPE_FLOAT:
                    case MYSQL_TYPE_DOUBLE:
                    case MYSQL_TYPE_DECIMAL:
                    case MYSQL_TYPE_NEWDECIMAL:
                        JS_SetPropertyStr(ctx, obj, fields[i].name,
                            JS_NewFloat64(ctx, atof(row[i])));
                        break;
                    default:
                        JS_SetPropertyStr(ctx, obj, fields[i].name,
                            JS_NewStringLen(ctx, row[i], row_lengths[i]));
                        break;
                }
            }
        }

        JS_SetPropertyUint32(ctx, results, idx++, obj);
    }

    return results;
}

/* -------------------- JS API: conn.query() -------------------- */

static JSValue js_mysql_query(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "query requires a SQL string");
    }

    MYSQL *conn = get_conn(ctx, this_val);
    if (!conn) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql) return JS_EXCEPTION;

    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        MYSQL_STMT *stmt = mysql_stmt_init(conn);
        if (!stmt) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_error(conn));
        }

        if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
            JS_FreeCString(ctx, sql);
            JSValue err = JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            return err;
        }
        JS_FreeCString(ctx, sql);

        int param_count = 0;
        unsigned long *lengths = NULL;
        char *null_flags = NULL;
        MYSQL_BIND *binds = bind_params(ctx, argv[1], &param_count, &lengths, &null_flags);

        if (binds && mysql_stmt_bind_param(stmt, binds)) {
            JSValue err = JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_stmt_error(stmt));
            free_binds(binds, param_count, lengths, null_flags);
            mysql_stmt_close(stmt);
            return err;
        }

        if (mysql_stmt_execute(stmt)) {
            JSValue err = JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_stmt_error(stmt));
            free_binds(binds, param_count, lengths, null_flags);
            mysql_stmt_close(stmt);
            return err;
        }

        free_binds(binds, param_count, lengths, null_flags);

        MYSQL_RES *meta = mysql_stmt_result_metadata(stmt);
        if (!meta) {
            mysql_stmt_close(stmt);
            return JS_NewArray(ctx);
        }

        int num_fields = mysql_num_fields(meta);
        MYSQL_FIELD *fields = mysql_fetch_fields(meta);

        MYSQL_BIND *result_binds = calloc(num_fields, sizeof(MYSQL_BIND));
        char **result_buffers = calloc(num_fields, sizeof(char *));
        unsigned long *result_lengths = calloc(num_fields, sizeof(unsigned long));
        bool *result_nulls = calloc(num_fields, sizeof(bool));

        for (int i = 0; i < num_fields; i++) {
            result_buffers[i] = calloc(1, 65536);
            result_binds[i].buffer_type = MYSQL_TYPE_STRING;
            result_binds[i].buffer = result_buffers[i];
            result_binds[i].buffer_length = 65536;
            result_binds[i].length = &result_lengths[i];
            result_binds[i].is_null = &result_nulls[i];
        }

        mysql_stmt_bind_result(stmt, result_binds);
        mysql_stmt_store_result(stmt);

        JSValue results = JS_NewArray(ctx);
        uint32_t idx = 0;

        while (mysql_stmt_fetch(stmt) == 0) {
            JSValue obj = JS_NewObject(ctx);
            for (int i = 0; i < num_fields; i++) {
                if (result_nulls[i]) {
                    JS_SetPropertyStr(ctx, obj, fields[i].name, JS_NULL);
                } else {
                    switch (fields[i].type) {
                        case MYSQL_TYPE_TINY:
                        case MYSQL_TYPE_SHORT:
                        case MYSQL_TYPE_LONG:
                        case MYSQL_TYPE_INT24:
                        case MYSQL_TYPE_LONGLONG:
                            JS_SetPropertyStr(ctx, obj, fields[i].name,
                                JS_NewInt64(ctx, atoll(result_buffers[i])));
                            break;
                        case MYSQL_TYPE_FLOAT:
                        case MYSQL_TYPE_DOUBLE:
                        case MYSQL_TYPE_DECIMAL:
                        case MYSQL_TYPE_NEWDECIMAL:
                            JS_SetPropertyStr(ctx, obj, fields[i].name,
                                JS_NewFloat64(ctx, atof(result_buffers[i])));
                            break;
                        default:
                            JS_SetPropertyStr(ctx, obj, fields[i].name,
                                JS_NewStringLen(ctx, result_buffers[i], result_lengths[i]));
                            break;
                    }
                }
            }
            JS_SetPropertyUint32(ctx, results, idx++, obj);
        }

        for (int i = 0; i < num_fields; i++) free(result_buffers[i]);
        free(result_buffers);
        free(result_lengths);
        free(result_nulls);
        free(result_binds);
        mysql_free_result(meta);
        mysql_stmt_close(stmt);

        return results;
    }

    /* No params — simple query */
    if (mysql_real_query(conn, sql, strlen(sql))) {
        JS_FreeCString(ctx, sql);
        return JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_error(conn));
    }
    JS_FreeCString(ctx, sql);

    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) {
        if (mysql_field_count(conn) == 0) {
            return JS_NewArray(ctx);
        }
        return JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_error(conn));
    }

    JSValue results = result_to_array(ctx, res);
    mysql_free_result(res);
    return results;
}

/* -------------------- JS API: conn.queryOne() -------------------- */

static JSValue js_mysql_query_one(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue results = js_mysql_query(ctx, this_val, argc, argv);
    if (JS_IsException(results)) return results;

    JSValue first = JS_GetPropertyUint32(ctx, results, 0);
    if (JS_IsUndefined(first)) {
        JS_FreeValue(ctx, results);
        return JS_NULL;
    }

    JS_FreeValue(ctx, results);
    return first;
}

/* -------------------- JS API: conn.exec() -------------------- */

static JSValue js_mysql_exec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "exec requires a SQL string");
    }

    MYSQL *conn = get_conn(ctx, this_val);
    if (!conn) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql) return JS_EXCEPTION;

    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        MYSQL_STMT *stmt = mysql_stmt_init(conn);
        if (!stmt) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_error(conn));
        }

        if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
            JS_FreeCString(ctx, sql);
            JSValue err = JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            return err;
        }
        JS_FreeCString(ctx, sql);

        int param_count = 0;
        unsigned long *lengths = NULL;
        char *null_flags = NULL;
        MYSQL_BIND *binds = bind_params(ctx, argv[1], &param_count, &lengths, &null_flags);

        if (binds && mysql_stmt_bind_param(stmt, binds)) {
            JSValue err = JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_stmt_error(stmt));
            free_binds(binds, param_count, lengths, null_flags);
            mysql_stmt_close(stmt);
            return err;
        }

        if (mysql_stmt_execute(stmt)) {
            JSValue err = JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_stmt_error(stmt));
            free_binds(binds, param_count, lengths, null_flags);
            mysql_stmt_close(stmt);
            return err;
        }

        free_binds(binds, param_count, lengths, null_flags);

        int id = get_conn_id(ctx, this_val);
        g_last_affected[id] = mysql_stmt_affected_rows(stmt);
        g_last_insert_id[id] = mysql_stmt_insert_id(stmt);
        mysql_stmt_close(stmt);
    } else {
        if (mysql_real_query(conn, sql, strlen(sql))) {
            JS_FreeCString(ctx, sql);
            return JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_error(conn));
        }
        JS_FreeCString(ctx, sql);

        int id = get_conn_id(ctx, this_val);
        g_last_affected[id] = mysql_affected_rows(conn);
        g_last_insert_id[id] = mysql_insert_id(conn);
    }

    return JS_UNDEFINED;
}

/* -------------------- JS API: conn.lastInsertId() -------------------- */

static JSValue js_mysql_last_insert_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = get_conn_id(ctx, this_val);
    if (id < 0 || id >= MAX_CONNS || !g_conns[id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }
    return JS_NewInt64(ctx, g_last_insert_id[id]);
}

/* -------------------- JS API: conn.changes() -------------------- */

static JSValue js_mysql_changes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = get_conn_id(ctx, this_val);
    if (id < 0 || id >= MAX_CONNS || !g_conns[id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }
    return JS_NewInt64(ctx, g_last_affected[id]);
}

/* -------------------- JS API: conn.beginTransaction() -------------------- */

static JSValue js_mysql_begin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    MYSQL *conn = get_conn(ctx, this_val);
    if (!conn) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }
    if (mysql_real_query(conn, "START TRANSACTION", 17)) {
        return JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_error(conn));
    }
    return JS_UNDEFINED;
}

/* -------------------- JS API: conn.commit() -------------------- */

static JSValue js_mysql_commit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    MYSQL *conn = get_conn(ctx, this_val);
    if (!conn) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }
    if (mysql_commit(conn)) {
        return JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_error(conn));
    }
    return JS_UNDEFINED;
}

/* -------------------- JS API: conn.rollback() -------------------- */

static JSValue js_mysql_rollback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    MYSQL *conn = get_conn(ctx, this_val);
    if (!conn) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }
    if (mysql_rollback(conn)) {
        return JS_ThrowTypeError(ctx, "MySQL error: %s", mysql_error(conn));
    }
    return JS_UNDEFINED;
}

/* -------------------- JS API: conn.close() -------------------- */

static JSValue js_mysql_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = get_conn_id(ctx, this_val);
    if (id >= 0 && id < MAX_CONNS && g_conns[id]) {
        mysql_close(g_conns[id]);
        g_conns[id] = NULL;
    }
    return JS_UNDEFINED;
}

/* -------------------- JS API: mysql.connect() -------------------- */

static JSValue js_mysql_connect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 4) {
        return JS_ThrowTypeError(ctx, "mysql.connect requires host, user, password, database");
    }

    int slot = find_conn_slot();
    if (slot < 0) {
        return JS_ThrowTypeError(ctx, "Maximum number of MySQL connections reached");
    }

    const char *host = JS_ToCString(ctx, argv[0]);
    const char *user = JS_ToCString(ctx, argv[1]);
    const char *pass = JS_ToCString(ctx, argv[2]);
    const char *dbname = JS_ToCString(ctx, argv[3]);

    unsigned int port = 3306;
    if (argc > 4) {
        int32_t p;
        JS_ToInt32(ctx, &p, argv[4]);
        port = (unsigned int)p;
    }

    if (!host || !user || !pass || !dbname) {
        if (host) JS_FreeCString(ctx, host);
        if (user) JS_FreeCString(ctx, user);
        if (pass) JS_FreeCString(ctx, pass);
        if (dbname) JS_FreeCString(ctx, dbname);
        return JS_EXCEPTION;
    }

    MYSQL *conn = mysql_init(NULL);
    if (!conn) {
        JS_FreeCString(ctx, host);
        JS_FreeCString(ctx, user);
        JS_FreeCString(ctx, pass);
        JS_FreeCString(ctx, dbname);
        return JS_ThrowTypeError(ctx, "MySQL init failed");
    }

    if (!mysql_real_connect(conn, host, user, pass, dbname, port, NULL, 0)) {
        JSValue err = JS_ThrowTypeError(ctx, "MySQL connect failed: %s", mysql_error(conn));
        mysql_close(conn);
        JS_FreeCString(ctx, host);
        JS_FreeCString(ctx, user);
        JS_FreeCString(ctx, pass);
        JS_FreeCString(ctx, dbname);
        return err;
    }

    JS_FreeCString(ctx, host);
    JS_FreeCString(ctx, user);
    JS_FreeCString(ctx, pass);
    JS_FreeCString(ctx, dbname);

    mysql_set_character_set(conn, "utf8mb4");

    g_conns[slot] = conn;
    if (slot >= g_conn_count) g_conn_count = slot + 1;

    /* Build the connection object */
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "_id", JS_NewInt32(ctx, slot));
    JS_SetPropertyStr(ctx, obj, "query",
        JS_NewCFunction(ctx, js_mysql_query, "query", 2));
    JS_SetPropertyStr(ctx, obj, "queryOne",
        JS_NewCFunction(ctx, js_mysql_query_one, "queryOne", 2));
    JS_SetPropertyStr(ctx, obj, "exec",
        JS_NewCFunction(ctx, js_mysql_exec, "exec", 2));
    JS_SetPropertyStr(ctx, obj, "lastInsertId",
        JS_NewCFunction(ctx, js_mysql_last_insert_id, "lastInsertId", 0));
    JS_SetPropertyStr(ctx, obj, "changes",
        JS_NewCFunction(ctx, js_mysql_changes, "changes", 0));
    JS_SetPropertyStr(ctx, obj, "beginTransaction",
        JS_NewCFunction(ctx, js_mysql_begin, "beginTransaction", 0));
    JS_SetPropertyStr(ctx, obj, "commit",
        JS_NewCFunction(ctx, js_mysql_commit, "commit", 0));
    JS_SetPropertyStr(ctx, obj, "rollback",
        JS_NewCFunction(ctx, js_mysql_rollback, "rollback", 0));
    JS_SetPropertyStr(ctx, obj, "close",
        JS_NewCFunction(ctx, js_mysql_close, "close", 0));

    return obj;
}

/* -------------------- Module entry -------------------- */

static int mysql_ext_init(JSContext *ctx, JSValue global) {
    memset(g_conns, 0, sizeof(g_conns));
    memset(g_last_affected, 0, sizeof(g_last_affected));
    memset(g_last_insert_id, 0, sizeof(g_last_insert_id));

    JSValue mysql_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, mysql_obj, "connect",
        JS_NewCFunction(ctx, js_mysql_connect, "connect", 5));
    JS_SetPropertyStr(ctx, global, "mysql", mysql_obj);

    return 0;
}

static int mysql_ext_shutdown(void) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_conns[i]) {
            mysql_close(g_conns[i]);
            g_conns[i] = NULL;
        }
    }
    return 0;
}

JSCGI_MODULE(mysql, "0.1.0", mysql_ext_init, mysql_ext_shutdown);
