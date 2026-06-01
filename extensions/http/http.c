#include "../../js-cgi-module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

/* -------------------- Helpers -------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buffer;

static void buffer_init(http_buffer *buf) {
    buf->data = malloc(4096);
    buf->len = 0;
    buf->cap = 4096;
    buf->data[0] = '\0';
}

static void buffer_free(http_buffer *buf) {
    free(buf->data);
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    http_buffer *buf = (http_buffer *)userp;

    while (buf->len + total + 1 > buf->cap) {
        buf->cap *= 2;
        buf->data = realloc(buf->data, buf->cap);
    }

    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static JSValue perform_request(JSContext *ctx, const char *method, const char *url,
                               const char *body, size_t body_len,
                               JSValue headers_obj) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        return JS_ThrowTypeError(ctx, "Failed to initialize HTTP client");
    }

    http_buffer response_body;
    buffer_init(&response_body);

    http_buffer response_headers;
    buffer_init(&response_headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

    /* Set request body if provided */
    if (body && body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }

    /* Set custom headers */
    struct curl_slist *header_list = NULL;
    if (JS_IsObject(headers_obj)) {
        JSPropertyEnum *tab;
        uint32_t tab_len;
        if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, headers_obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < tab_len; i++) {
                const char *key = JS_AtomToCString(ctx, tab[i].atom);
                JSValue val = JS_GetProperty(ctx, headers_obj, tab[i].atom);
                const char *val_str = JS_ToCString(ctx, val);

                if (key && val_str) {
                    char header[2048];
                    snprintf(header, sizeof(header), "%s: %s", key, val_str);
                    header_list = curl_slist_append(header_list, header);
                }

                if (val_str) JS_FreeCString(ctx, val_str);
                if (key) JS_FreeCString(ctx, key);
                JS_FreeValue(ctx, val);
                JS_FreeAtom(ctx, tab[i].atom);
            }
            js_free(ctx, tab);
        }
    }

    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    CURLcode res = curl_easy_perform(curl);

    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        buffer_free(&response_body);
        buffer_free(&response_headers);
        return JS_ThrowTypeError(ctx, "HTTP request failed: %s", curl_easy_strerror(res));
    }

    /* Build response object */
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, (int)status_code));
    JS_SetPropertyStr(ctx, result, "body", JS_NewStringLen(ctx, response_body.data, response_body.len));

    /* Parse response headers */
    JSValue resp_headers = JS_NewObject(ctx);
    char *line = strtok(response_headers.data, "\r\n");
    while (line) {
        char *colon = strchr(line, ':');
        if (colon && line[0] != 'H') { /* skip HTTP/1.1 status line */
            *colon = '\0';
            char *val = colon + 1;
            while (*val == ' ') val++;

            /* Lowercase the header name */
            for (char *p = line; *p; p++) {
                if (*p >= 'A' && *p <= 'Z') *p += 32;
            }

            JS_SetPropertyStr(ctx, resp_headers, line, JS_NewString(ctx, val));
        }
        line = strtok(NULL, "\r\n");
    }
    JS_SetPropertyStr(ctx, result, "headers", resp_headers);

    buffer_free(&response_body);
    buffer_free(&response_headers);

    return result;
}

/* -------------------- JS API: http.get() -------------------- */

static JSValue js_http_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "http.get requires a URL");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    JSValue headers = (argc > 1 && JS_IsObject(argv[1])) ? argv[1] : JS_UNDEFINED;

    JSValue result = perform_request(ctx, "GET", url, NULL, 0, headers);
    JS_FreeCString(ctx, url);
    return result;
}

/* -------------------- JS API: http.post() -------------------- */

static JSValue js_http_post(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "http.post requires a URL");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    const char *body = NULL;
    size_t body_len = 0;
    if (argc > 1 && JS_IsString(argv[1])) {
        body = JS_ToCStringLen(ctx, &body_len, argv[1]);
    }

    JSValue headers = (argc > 2 && JS_IsObject(argv[2])) ? argv[2] : JS_UNDEFINED;

    JSValue result = perform_request(ctx, "POST", url, body, body_len, headers);
    JS_FreeCString(ctx, url);
    if (body) JS_FreeCString(ctx, body);
    return result;
}

/* -------------------- JS API: http.put() -------------------- */

static JSValue js_http_put(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "http.put requires a URL");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    const char *body = NULL;
    size_t body_len = 0;
    if (argc > 1 && JS_IsString(argv[1])) {
        body = JS_ToCStringLen(ctx, &body_len, argv[1]);
    }

    JSValue headers = (argc > 2 && JS_IsObject(argv[2])) ? argv[2] : JS_UNDEFINED;

    JSValue result = perform_request(ctx, "PUT", url, body, body_len, headers);
    JS_FreeCString(ctx, url);
    if (body) JS_FreeCString(ctx, body);
    return result;
}

/* -------------------- JS API: http.delete() -------------------- */

static JSValue js_http_del(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "http.delete requires a URL");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;

    JSValue headers = (argc > 1 && JS_IsObject(argv[1])) ? argv[1] : JS_UNDEFINED;

    JSValue result = perform_request(ctx, "DELETE", url, NULL, 0, headers);
    JS_FreeCString(ctx, url);
    return result;
}

/* -------------------- Module entry -------------------- */

static int http_init(JSContext *ctx, JSValue global) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    JSValue http_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, http_obj, "get",
        JS_NewCFunction(ctx, js_http_get, "get", 2));
    JS_SetPropertyStr(ctx, http_obj, "post",
        JS_NewCFunction(ctx, js_http_post, "post", 3));
    JS_SetPropertyStr(ctx, http_obj, "put",
        JS_NewCFunction(ctx, js_http_put, "put", 3));
    JS_SetPropertyStr(ctx, http_obj, "delete",
        JS_NewCFunction(ctx, js_http_del, "delete", 2));
    JS_SetPropertyStr(ctx, global, "http", http_obj);

    return 0;
}

static int http_shutdown(void) {
    curl_global_cleanup();
    return 0;
}

JSCGI_MODULE(http, "0.1.0", http_init, http_shutdown);
