#include "../../js-cgi-module.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern char g_script_dir[4096];

static const char *resolve_path(const char *path, char *buf, size_t buf_size) {
    if (path[0] == '/') return path;
    snprintf(buf, buf_size, "%s/%s", g_script_dir, path);
    return buf;
}

#define MAX_DBS 16

static sqlite3 *g_dbs[MAX_DBS];
static int g_db_count = 0;

/* -------------------- Helpers -------------------- */

static int find_db_slot(void) {
    for (int i = 0; i < MAX_DBS; i++) {
        if (!g_dbs[i]) return i;
    }
    return -1;
}

static void bind_params(sqlite3_stmt *stmt, JSContext *ctx, JSValue params) {
    if (JS_IsUndefined(params) || JS_IsNull(params)) return;

    JSValue length_val = JS_GetPropertyStr(ctx, params, "length");
    int64_t len = 0;
    JS_ToInt64(ctx, &len, length_val);
    JS_FreeValue(ctx, length_val);

    for (int i = 0; i < len; i++) {
        JSValue val = JS_GetPropertyUint32(ctx, params, i);
        int tag = JS_VALUE_GET_NORM_TAG(val);

        switch (tag) {
            case JS_TAG_INT:
                sqlite3_bind_int64(stmt, i + 1, JS_VALUE_GET_INT(val));
                break;
            case JS_TAG_FLOAT64:
                sqlite3_bind_double(stmt, i + 1, JS_VALUE_GET_FLOAT64(val));
                break;
            case JS_TAG_STRING: {
                const char *str = JS_ToCString(ctx, val);
                sqlite3_bind_text(stmt, i + 1, str, -1, SQLITE_TRANSIENT);
                JS_FreeCString(ctx, str);
                break;
            }
            case JS_TAG_NULL:
            case JS_TAG_UNDEFINED:
                sqlite3_bind_null(stmt, i + 1);
                break;
            case JS_TAG_OBJECT: {
                /* Treat arrays as BLOBs */
                if (JS_IsArray(val)) {
                    JSValue len_val = JS_GetPropertyStr(ctx, val, "length");
                    int64_t blob_len = 0;
                    JS_ToInt64(ctx, &blob_len, len_val);
                    JS_FreeValue(ctx, len_val);

                    unsigned char *blob = malloc(blob_len);
                    for (int j = 0; j < blob_len; j++) {
                        JSValue byte_val = JS_GetPropertyUint32(ctx, val, j);
                        int32_t byte;
                        JS_ToInt32(ctx, &byte, byte_val);
                        blob[j] = (unsigned char)byte;
                        JS_FreeValue(ctx, byte_val);
                    }
                    sqlite3_bind_blob(stmt, i + 1, blob, blob_len, SQLITE_TRANSIENT);
                    free(blob);
                } else {
                    sqlite3_bind_null(stmt, i + 1);
                }
                break;
            }
            default:
                sqlite3_bind_null(stmt, i + 1);
                break;
        }

        JS_FreeValue(ctx, val);
    }
}

static JSValue row_to_object(JSContext *ctx, sqlite3_stmt *stmt) {
    JSValue obj = JS_NewObject(ctx);
    int cols = sqlite3_column_count(stmt);

    for (int i = 0; i < cols; i++) {
        const char *name = sqlite3_column_name(stmt, i);
        int type = sqlite3_column_type(stmt, i);

        switch (type) {
            case SQLITE_INTEGER:
                JS_SetPropertyStr(ctx, obj, name,
                    JS_NewInt64(ctx, sqlite3_column_int64(stmt, i)));
                break;
            case SQLITE_FLOAT:
                JS_SetPropertyStr(ctx, obj, name,
                    JS_NewFloat64(ctx, sqlite3_column_double(stmt, i)));
                break;
            case SQLITE_TEXT:
                JS_SetPropertyStr(ctx, obj, name,
                    JS_NewString(ctx, (const char *)sqlite3_column_text(stmt, i)));
                break;
            case SQLITE_BLOB: {
                const void *blob = sqlite3_column_blob(stmt, i);
                int blob_len = sqlite3_column_bytes(stmt, i);
                JSValue arr = JS_NewArray(ctx);
                const unsigned char *bytes = (const unsigned char *)blob;
                for (int j = 0; j < blob_len; j++) {
                    JS_SetPropertyUint32(ctx, arr, j, JS_NewInt32(ctx, bytes[j]));
                }
                JS_SetPropertyStr(ctx, obj, name, arr);
                break;
            }
            case SQLITE_NULL:
                JS_SetPropertyStr(ctx, obj, name, JS_NULL);
                break;
            default:
                JS_SetPropertyStr(ctx, obj, name, JS_NULL);
                break;
        }
    }

    return obj;
}

/* -------------------- JS API: db.exec() -------------------- */

static JSValue js_db_exec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "exec requires a SQL string");
    }

    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id < 0 || db_id >= MAX_DBS || !g_dbs[db_id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql) return JS_EXCEPTION;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_dbs[db_id], sql, -1, &stmt, NULL);
    JS_FreeCString(ctx, sql);

    if (rc != SQLITE_OK) {
        return JS_ThrowTypeError(ctx, "SQL error: %s", sqlite3_errmsg(g_dbs[db_id]));
    }

    if (argc > 1) {
        bind_params(stmt, ctx, argv[1]);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return JS_ThrowTypeError(ctx, "SQL error: %s", sqlite3_errmsg(g_dbs[db_id]));
    }

    return JS_UNDEFINED;
}

/* -------------------- JS API: db.query() -------------------- */

static JSValue js_db_query(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "query requires a SQL string");
    }

    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id < 0 || db_id >= MAX_DBS || !g_dbs[db_id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql) return JS_EXCEPTION;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_dbs[db_id], sql, -1, &stmt, NULL);
    JS_FreeCString(ctx, sql);

    if (rc != SQLITE_OK) {
        return JS_ThrowTypeError(ctx, "SQL error: %s", sqlite3_errmsg(g_dbs[db_id]));
    }

    if (argc > 1) {
        bind_params(stmt, ctx, argv[1]);
    }

    JSValue results = JS_NewArray(ctx);
    uint32_t idx = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        JS_SetPropertyUint32(ctx, results, idx++, row_to_object(ctx, stmt));
    }

    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        JS_FreeValue(ctx, results);
        return JS_ThrowTypeError(ctx, "SQL error: %s", sqlite3_errmsg(g_dbs[db_id]));
    }

    return results;
}

/* -------------------- JS API: db.queryOne() -------------------- */

static JSValue js_db_query_one(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "queryOne requires a SQL string");
    }

    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id < 0 || db_id >= MAX_DBS || !g_dbs[db_id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    const char *sql = JS_ToCString(ctx, argv[0]);
    if (!sql) return JS_EXCEPTION;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_dbs[db_id], sql, -1, &stmt, NULL);
    JS_FreeCString(ctx, sql);

    if (rc != SQLITE_OK) {
        return JS_ThrowTypeError(ctx, "SQL error: %s", sqlite3_errmsg(g_dbs[db_id]));
    }

    if (argc > 1) {
        bind_params(stmt, ctx, argv[1]);
    }

    rc = sqlite3_step(stmt);

    JSValue result;
    if (rc == SQLITE_ROW) {
        result = row_to_object(ctx, stmt);
    } else {
        result = JS_NULL;
    }

    sqlite3_finalize(stmt);
    return result;
}

/* -------------------- JS API: db.lastInsertId() -------------------- */

static JSValue js_db_last_insert_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id < 0 || db_id >= MAX_DBS || !g_dbs[db_id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    return JS_NewInt64(ctx, sqlite3_last_insert_rowid(g_dbs[db_id]));
}

/* -------------------- JS API: db.changes() -------------------- */

static JSValue js_db_changes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id < 0 || db_id >= MAX_DBS || !g_dbs[db_id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    return JS_NewInt32(ctx, sqlite3_changes(g_dbs[db_id]));
}

/* -------------------- JS API: db.beginTransaction() -------------------- */

static JSValue js_db_begin(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id < 0 || db_id >= MAX_DBS || !g_dbs[db_id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    char *err = NULL;
    int rc = sqlite3_exec(g_dbs[db_id], "BEGIN TRANSACTION", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        JSValue ex = JS_ThrowTypeError(ctx, "Transaction error: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
        return ex;
    }

    return JS_UNDEFINED;
}

/* -------------------- JS API: db.commit() -------------------- */

static JSValue js_db_commit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id < 0 || db_id >= MAX_DBS || !g_dbs[db_id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    char *err = NULL;
    int rc = sqlite3_exec(g_dbs[db_id], "COMMIT", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        JSValue ex = JS_ThrowTypeError(ctx, "Commit error: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
        return ex;
    }

    return JS_UNDEFINED;
}

/* -------------------- JS API: db.rollback() -------------------- */

static JSValue js_db_rollback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id < 0 || db_id >= MAX_DBS || !g_dbs[db_id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    char *err = NULL;
    int rc = sqlite3_exec(g_dbs[db_id], "ROLLBACK", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        JSValue ex = JS_ThrowTypeError(ctx, "Rollback error: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
        return ex;
    }

    return JS_UNDEFINED;
}

/* -------------------- JS API: db.tableExists() -------------------- */

static JSValue js_db_table_exists(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "tableExists requires a table name");
    }

    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id < 0 || db_id >= MAX_DBS || !g_dbs[db_id]) {
        return JS_ThrowTypeError(ctx, "Database connection is closed");
    }

    const char *table = JS_ToCString(ctx, argv[0]);
    if (!table) return JS_EXCEPTION;

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(g_dbs[db_id],
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
        -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        JS_FreeCString(ctx, table);
        return JS_ThrowTypeError(ctx, "SQL error: %s", sqlite3_errmsg(g_dbs[db_id]));
    }

    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    JS_FreeCString(ctx, table);

    rc = sqlite3_step(stmt);
    int exists = (rc == SQLITE_ROW);
    sqlite3_finalize(stmt);

    return JS_NewBool(ctx, exists);
}

/* -------------------- JS API: db.close() -------------------- */

static JSValue js_db_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue db_id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int db_id = JS_VALUE_GET_INT(db_id_val);
    JS_FreeValue(ctx, db_id_val);

    if (db_id >= 0 && db_id < MAX_DBS && g_dbs[db_id]) {
        sqlite3_close(g_dbs[db_id]);
        g_dbs[db_id] = NULL;
    }

    return JS_UNDEFINED;
}

/* -------------------- JS API: sqlite.open() -------------------- */

static JSValue js_sqlite_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "sqlite.open requires a file path");
    }

    int slot = find_db_slot();
    if (slot < 0) {
        return JS_ThrowTypeError(ctx, "Maximum number of database connections reached");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char resolved[8192];
    const char *full_path = resolve_path(path, resolved, sizeof(resolved));

    int rc = sqlite3_open(full_path, &g_dbs[slot]);
    JS_FreeCString(ctx, path);

    if (rc != SQLITE_OK) {
        const char *err = sqlite3_errmsg(g_dbs[slot]);
        sqlite3_close(g_dbs[slot]);
        g_dbs[slot] = NULL;
        return JS_ThrowTypeError(ctx, "Cannot open database: %s", err);
    }

    if (slot >= g_db_count) g_db_count = slot + 1;

    /* Build the db object */
    JSValue db = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, db, "_id", JS_NewInt32(ctx, slot));
    JS_SetPropertyStr(ctx, db, "exec",
        JS_NewCFunction(ctx, js_db_exec, "exec", 2));
    JS_SetPropertyStr(ctx, db, "query",
        JS_NewCFunction(ctx, js_db_query, "query", 2));
    JS_SetPropertyStr(ctx, db, "queryOne",
        JS_NewCFunction(ctx, js_db_query_one, "queryOne", 2));
    JS_SetPropertyStr(ctx, db, "tableExists",
        JS_NewCFunction(ctx, js_db_table_exists, "tableExists", 1));
    JS_SetPropertyStr(ctx, db, "lastInsertId",
        JS_NewCFunction(ctx, js_db_last_insert_id, "lastInsertId", 0));
    JS_SetPropertyStr(ctx, db, "changes",
        JS_NewCFunction(ctx, js_db_changes, "changes", 0));
    JS_SetPropertyStr(ctx, db, "beginTransaction",
        JS_NewCFunction(ctx, js_db_begin, "beginTransaction", 0));
    JS_SetPropertyStr(ctx, db, "commit",
        JS_NewCFunction(ctx, js_db_commit, "commit", 0));
    JS_SetPropertyStr(ctx, db, "rollback",
        JS_NewCFunction(ctx, js_db_rollback, "rollback", 0));
    JS_SetPropertyStr(ctx, db, "close",
        JS_NewCFunction(ctx, js_db_close, "close", 0));

    return db;
}

/* -------------------- Module entry -------------------- */

static int sqlite_init(JSContext *ctx, JSValue global) {
    memset(g_dbs, 0, sizeof(g_dbs));

    JSValue sqlite_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, sqlite_obj, "open",
        JS_NewCFunction(ctx, js_sqlite_open, "open", 1));
    JS_SetPropertyStr(ctx, global, "sqlite", sqlite_obj);

    return 0;
}

static int sqlite_shutdown(void) {
    for (int i = 0; i < MAX_DBS; i++) {
        if (g_dbs[i]) {
            sqlite3_close(g_dbs[i]);
            g_dbs[i] = NULL;
        }
    }
    return 0;
}

JSCGI_MODULE(sqlite, "0.1.0", sqlite_init, sqlite_shutdown);
