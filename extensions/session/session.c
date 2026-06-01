#include "../../js-cgi-module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define SESSION_COOKIE_NAME "JSCGI_SESSID"
#define SESSION_DIR "/tmp/js-cgi_sessions"
#define SESSION_ID_LEN 32

static char g_session_id[SESSION_ID_LEN + 1];
static char g_session_file[4096];
static JSValue g_session_data = JS_UNDEFINED;
static JSContext *g_ctx = NULL;
static int g_session_started = 0;

/* -------------------- Helpers -------------------- */

static void generate_session_id(void) {
    static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    FILE *fp = fopen("/dev/urandom", "rb");
    unsigned char rand_bytes[SESSION_ID_LEN];

    if (fp) {
        fread(rand_bytes, 1, SESSION_ID_LEN, fp);
        fclose(fp);
    } else {
        srand(time(NULL) ^ getpid());
        for (int i = 0; i < SESSION_ID_LEN; i++) {
            rand_bytes[i] = rand();
        }
    }

    for (int i = 0; i < SESSION_ID_LEN; i++) {
        g_session_id[i] = chars[rand_bytes[i] % (sizeof(chars) - 1)];
    }
    g_session_id[SESSION_ID_LEN] = '\0';
}

static const char *get_cookie_value(const char *cookie_header, const char *name) {
    if (!cookie_header) return NULL;

    size_t name_len = strlen(name);
    const char *p = cookie_header;

    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;

        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            return p + name_len + 1;
        }

        while (*p && *p != ';') p++;
    }

    return NULL;
}

static char *read_file_contents(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    size_t read = fread(buf, 1, len, fp);
    buf[read] = '\0';
    fclose(fp);
    return buf;
}

static void write_file_contents(const char *path, const char *data) {
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fputs(data, fp);
        fclose(fp);
    }
}

static void ensure_session_dir(void) {
    struct stat st;
    if (stat(SESSION_DIR, &st) != 0) {
        mkdir(SESSION_DIR, 0700);
    }
}

/* -------------------- JS API -------------------- */

static JSValue js_session_start(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (g_session_started) return JS_UNDEFINED;

    ensure_session_dir();

    /* Check for existing session cookie */
    const char *cookie_header = getenv("HTTP_COOKIE");
    const char *existing_id = get_cookie_value(cookie_header, SESSION_COOKIE_NAME);

    int new_session = 0;

    if (existing_id && strlen(existing_id) >= SESSION_ID_LEN) {
        /* Extract session ID (up to semicolon or end) */
        int i;
        for (i = 0; i < SESSION_ID_LEN && existing_id[i] && existing_id[i] != ';'; i++) {
            g_session_id[i] = existing_id[i];
        }
        g_session_id[i] = '\0';
    } else {
        generate_session_id();
        new_session = 1;
    }

    /* Build session file path */
    snprintf(g_session_file, sizeof(g_session_file), "%s/sess_%s", SESSION_DIR, g_session_id);

    /* Load existing session data or create empty object */
    if (!new_session) {
        char *json = read_file_contents(g_session_file);
        if (json) {
            g_session_data = JS_ParseJSON(ctx, json, strlen(json), "<session>");
            free(json);
            if (JS_IsException(g_session_data)) {
                g_session_data = JS_NewObject(ctx);
            }
        } else {
            g_session_data = JS_NewObject(ctx);
            new_session = 1;
        }
    } else {
        g_session_data = JS_NewObject(ctx);
    }

    /* Set the cookie header via printf to stdout (before body) */
    /* We need to use the response mechanism - output Set-Cookie header */
    extern void response_append_header(const char *header);
    char cookie_header_str[256];
    snprintf(cookie_header_str, sizeof(cookie_header_str),
        "Set-Cookie: %s=%s; Path=/; HttpOnly; SameSite=Strict",
        SESSION_COOKIE_NAME, g_session_id);
    response_append_header(cookie_header_str);

    g_ctx = ctx;
    g_session_started = 1;

    return JS_UNDEFINED;
}

static JSValue js_session_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!g_session_started) {
        return JS_ThrowTypeError(ctx, "Session not started. Call session.start() first");
    }

    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "session.get requires a key");
    }

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    JSValue val = JS_GetPropertyStr(ctx, g_session_data, key);
    JS_FreeCString(ctx, key);

    if (JS_IsUndefined(val)) {
        return JS_NULL;
    }

    return val;
}

static JSValue js_session_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!g_session_started) {
        return JS_ThrowTypeError(ctx, "Session not started. Call session.start() first");
    }

    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "session.set requires a key and value");
    }

    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;

    JS_SetPropertyStr(ctx, g_session_data, key, JS_DupValue(ctx, argv[1]));
    JS_FreeCString(ctx, key);

    /* Save session data to file */
    JSValue json = JS_JSONStringify(ctx, g_session_data, JS_UNDEFINED, JS_UNDEFINED);
    const char *json_str = JS_ToCString(ctx, json);
    if (json_str) {
        write_file_contents(g_session_file, json_str);
        JS_FreeCString(ctx, json_str);
    }
    JS_FreeValue(ctx, json);

    return JS_UNDEFINED;
}

static JSValue js_session_destroy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!g_session_started) {
        return JS_ThrowTypeError(ctx, "Session not started. Call session.start() first");
    }

    /* Delete session file */
    unlink(g_session_file);

    /* Clear data */
    JS_FreeValue(ctx, g_session_data);
    g_session_data = JS_NewObject(ctx);

    /* Expire the cookie */
    extern void response_append_header(const char *header);
    char cookie_header_str[256];
    snprintf(cookie_header_str, sizeof(cookie_header_str),
        "Set-Cookie: %s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0",
        SESSION_COOKIE_NAME);
    response_append_header(cookie_header_str);

    g_session_started = 0;

    return JS_UNDEFINED;
}

/* -------------------- Module entry -------------------- */

static int session_init(JSContext *ctx, JSValue global) {
    g_session_started = 0;
    g_session_id[0] = '\0';
    g_session_file[0] = '\0';
    g_session_data = JS_UNDEFINED;
    g_ctx = NULL;

    JSValue session_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, session_obj, "start",
        JS_NewCFunction(ctx, js_session_start, "start", 0));
    JS_SetPropertyStr(ctx, session_obj, "get",
        JS_NewCFunction(ctx, js_session_get, "get", 1));
    JS_SetPropertyStr(ctx, session_obj, "set",
        JS_NewCFunction(ctx, js_session_set, "set", 2));
    JS_SetPropertyStr(ctx, session_obj, "destroy",
        JS_NewCFunction(ctx, js_session_destroy, "destroy", 0));
    JS_SetPropertyStr(ctx, global, "session", session_obj);

    return 0;
}

static int session_shutdown(void) {
    if (g_ctx && !JS_IsUndefined(g_session_data)) {
        JS_FreeValue(g_ctx, g_session_data);
        g_session_data = JS_UNDEFINED;
    }
    g_session_started = 0;
    g_ctx = NULL;
    return 0;
}

JSCGI_MODULE(session, "0.1.0", session_init, session_shutdown);
