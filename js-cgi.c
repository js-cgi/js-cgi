#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>

#include "quickjs/quickjs.h"
#include "js-cgi-module.h"

/* Forward declarations */
static char g_script_dir[4096];
static char *read_file(const char *path, size_t *out_len);

#define DEFAULT_MEMORY_LIMIT (128 * 1024 * 1024)
#define DEFAULT_MAX_EXECUTION_TIME 30
#define DEFAULT_CONTENT_TYPE "text/html"
#define INI_SYSTEM_PATH "/etc/js-cgi/js-cgi.ini"
#define INI_MAX_LINE 1024
#define MAX_EXTENSIONS 64

/* -------------------- Configuration -------------------- */

typedef struct {
    size_t memory_limit;
    int max_execution_time;
    char extension_dir[4096];
    char *extensions[MAX_EXTENSIONS];
    int extension_count;
    int display_errors;
    char error_log[4096];
} jscgi_config;

static void config_init(jscgi_config *cfg) {
    cfg->memory_limit = DEFAULT_MEMORY_LIMIT;
    cfg->max_execution_time = DEFAULT_MAX_EXECUTION_TIME;
    strcpy(cfg->extension_dir, "/usr/lib/js-cgi/modules");
    cfg->extension_count = 0;
    cfg->display_errors = 1;
    cfg->error_log[0] = '\0';
}

static size_t parse_memory_value(const char *val) {
    size_t num = 0;
    char suffix = 0;
    int len = strlen(val);

    num = atol(val);
    if (len > 0) {
        suffix = val[len - 1];
        if (suffix == 'M' || suffix == 'm') {
            num *= 1024 * 1024;
        } else if (suffix == 'K' || suffix == 'k') {
            num *= 1024;
        } else if (suffix == 'G' || suffix == 'g') {
            num *= 1024 * 1024 * 1024;
        }
    }
    return num;
}

static int config_load_file(jscgi_config *cfg, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[INI_MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and empty lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\0') continue;

        /* Remove trailing newline */
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        /* Parse key = value */
        char *eq = strchr(p, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        /* Trim whitespace */
        while (*key == ' ' || *key == '\t') key++;
        char *end = key + strlen(key) - 1;
        while (end > key && (*end == ' ' || *end == '\t')) { *end = '\0'; end--; }

        while (*val == ' ' || *val == '\t') val++;
        end = val + strlen(val) - 1;
        while (end > val && (*end == ' ' || *end == '\t')) { *end = '\0'; end--; }

        if (strcmp(key, "memory_limit") == 0) {
            cfg->memory_limit = parse_memory_value(val);
        } else if (strcmp(key, "max_execution_time") == 0) {
            cfg->max_execution_time = atoi(val);
        } else if (strcmp(key, "extension_dir") == 0) {
            strncpy(cfg->extension_dir, val, sizeof(cfg->extension_dir) - 1);
        } else if (strcmp(key, "extension") == 0) {
            if (cfg->extension_count < MAX_EXTENSIONS) {
                cfg->extensions[cfg->extension_count++] = strdup(val);
            }
        } else if (strcmp(key, "display_errors") == 0) {
            cfg->display_errors = (strcasecmp(val, "On") == 0 || strcmp(val, "1") == 0);
        } else if (strcmp(key, "error_log") == 0) {
            strncpy(cfg->error_log, val, sizeof(cfg->error_log) - 1);
        }
    }

    fclose(fp);
    return 0;
}

static void config_load(jscgi_config *cfg, const char *ini_override) {
    config_init(cfg);

    if (ini_override) {
        config_load_file(cfg, ini_override);
        return;
    }

    /* System-wide first */
    config_load_file(cfg, INI_SYSTEM_PATH);

    /* Local override */
    char local_path[4096];
    ssize_t len = readlink("/proc/self/exe", local_path, sizeof(local_path) - 1);
    if (len > 0) {
        local_path[len] = '\0';
        char *slash = strrchr(local_path, '/');
        if (slash) {
            strcpy(slash + 1, "js-cgi.ini");
            config_load_file(cfg, local_path);
        }
    }
}

/* -------------------- Response state -------------------- */

typedef struct {
    char *headers;
    size_t headers_len;
    size_t headers_cap;
    char *body;
    size_t body_len;
    size_t body_cap;
    int status;
    int content_type_set;
} jscgi_response;

static jscgi_response g_response;

static void response_init(void) {
    g_response.headers = malloc(4096);
    g_response.headers_len = 0;
    g_response.headers_cap = 4096;
    g_response.body = malloc(65536);
    g_response.body_len = 0;
    g_response.body_cap = 65536;
    g_response.status = 200;
    g_response.content_type_set = 0;
}

static void response_free(void) {
    free(g_response.headers);
    free(g_response.body);
}

void response_append_header(const char *header) {
    size_t len = strlen(header);
    while (g_response.headers_len + len + 3 > g_response.headers_cap) {
        g_response.headers_cap *= 2;
        g_response.headers = realloc(g_response.headers, g_response.headers_cap);
    }
    memcpy(g_response.headers + g_response.headers_len, header, len);
    g_response.headers_len += len;
    g_response.headers[g_response.headers_len++] = '\r';
    g_response.headers[g_response.headers_len++] = '\n';
    g_response.headers[g_response.headers_len] = '\0';
}

static void response_append_body(const char *data, size_t len) {
    while (g_response.body_len + len + 1 > g_response.body_cap) {
        g_response.body_cap *= 2;
        g_response.body = realloc(g_response.body, g_response.body_cap);
    }
    memcpy(g_response.body + g_response.body_len, data, len);
    g_response.body_len += len;
    g_response.body[g_response.body_len] = '\0';
}

static void response_output(void) {
    /* Status line */
    printf("Status: %d\r\n", g_response.status);

    /* Default content-type if not set */
    if (!g_response.content_type_set) {
        printf("Content-Type: %s\r\n", DEFAULT_CONTENT_TYPE);
    }

    /* Custom headers */
    if (g_response.headers_len > 0) {
        fwrite(g_response.headers, 1, g_response.headers_len, stdout);
    }

    /* End of headers */
    printf("\r\n");

    /* Body */
    if (g_response.body_len > 0) {
        fwrite(g_response.body, 1, g_response.body_len, stdout);
    }
}

/* -------------------- Interrupt handler -------------------- */

static uint64_t g_start_time;
static int g_max_execution_time;

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int interrupt_handler(JSRuntime *rt, void *opaque) {
    if (g_max_execution_time <= 0) return 0;
    uint64_t elapsed = get_time_ms() - g_start_time;
    return elapsed >= (uint64_t)g_max_execution_time * 1000 ? 1 : 0;
}

/* -------------------- JS API: print() -------------------- */

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            response_append_body(str, strlen(str));
            JS_FreeCString(ctx, str);
        }
    }
    return JS_UNDEFINED;
}

/* -------------------- JS API: include() -------------------- */

static JSValue js_include(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "include requires a file path");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    /* Resolve relative to script directory */
    char resolved[8192];
    if (path[0] == '/') {
        snprintf(resolved, sizeof(resolved), "%s", path);
    } else {
        snprintf(resolved, sizeof(resolved), "%s/%s", g_script_dir, path);
    }
    JS_FreeCString(ctx, path);

    /* Resolve /./ in path */
    char cleaned[4096];
    char *src = resolved;
    char *dst = cleaned;
    while (*src) {
        if (src[0] == '/' && src[1] == '.' && src[2] == '/') {
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    /* Read the file */
    size_t file_len;
    char *source = read_file(cleaned, &file_len);
    if (!source) {
        return JS_ThrowReferenceError(ctx, "Cannot include '%s': file not found", cleaned);
    }

    /* Get filename for error reporting */
    const char *filename = strrchr(cleaned, '/');
    filename = filename ? filename + 1 : cleaned;

    /* Execute in the current context */
    JSValue result = JS_Eval(ctx, source, file_len, filename, JS_EVAL_TYPE_GLOBAL);
    free(source);

    if (JS_IsException(result)) {
        return result;
    }

    JS_FreeValue(ctx, result);
    return JS_UNDEFINED;
}

/* -------------------- JS API: response -------------------- */

static JSValue js_response_set_header(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;

    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);

    if (name && value) {
        char header[2048];
        snprintf(header, sizeof(header), "%s: %s", name, value);
        response_append_header(header);

        if (strcasecmp(name, "Content-Type") == 0) {
            g_response.content_type_set = 1;
        }
    }

    if (name) JS_FreeCString(ctx, name);
    if (value) JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue js_response_set_status(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;

    int status;
    JS_ToInt32(ctx, &status, argv[0]);
    g_response.status = status;
    return JS_UNDEFINED;
}

/* -------------------- JS API: request -------------------- */

static void parse_query_string(JSContext *ctx, JSValue obj, const char *qs) {
    if (!qs || !*qs) return;

    char *copy = strdup(qs);
    char *pair = strtok(copy, "&");

    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            JS_SetPropertyStr(ctx, obj, pair, JS_NewString(ctx, eq + 1));
        } else {
            JS_SetPropertyStr(ctx, obj, pair, JS_NewString(ctx, ""));
        }
        pair = strtok(NULL, "&");
    }

    free(copy);
}

static void parse_headers(JSContext *ctx, JSValue obj) {
    extern char **environ;
    for (char **env = environ; *env; env++) {
        if (strncmp(*env, "HTTP_", 5) == 0) {
            char *eq = strchr(*env, '=');
            if (!eq) continue;

            /* Convert HTTP_FOO_BAR to foo-bar */
            size_t name_len = eq - *env - 5;
            char *name = malloc(name_len + 1);
            for (size_t i = 0; i < name_len; i++) {
                char c = (*env)[i + 5];
                if (c == '_') name[i] = '-';
                else name[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            }
            name[name_len] = '\0';

            JS_SetPropertyStr(ctx, obj, name, JS_NewString(ctx, eq + 1));
            free(name);
        }
    }

    /* Content-Type and Content-Length aren't prefixed with HTTP_ */
    const char *ct = getenv("CONTENT_TYPE");
    if (ct) JS_SetPropertyStr(ctx, obj, "content-type", JS_NewString(ctx, ct));
    const char *cl = getenv("CONTENT_LENGTH");
    if (cl) JS_SetPropertyStr(ctx, obj, "content-length", JS_NewString(ctx, cl));
}

static char *read_request_body(void) {
    const char *cl_str = getenv("CONTENT_LENGTH");
    if (!cl_str) return strdup("");

    int content_length = atoi(cl_str);
    if (content_length <= 0) return strdup("");

    char *body = malloc(content_length + 1);
    size_t read_total = 0;
    while (read_total < (size_t)content_length) {
        size_t n = fread(body + read_total, 1, content_length - read_total, stdin);
        if (n == 0) break;
        read_total += n;
    }
    body[read_total] = '\0';
    return body;
}

static JSValue build_request_object(JSContext *ctx) {
    JSValue req = JS_NewObject(ctx);

    /* method */
    const char *method = getenv("REQUEST_METHOD");
    JS_SetPropertyStr(ctx, req, "method", JS_NewString(ctx, method ? method : "GET"));

    /* uri */
    const char *uri = getenv("REQUEST_URI");
    if (!uri) uri = getenv("SCRIPT_NAME");
    JS_SetPropertyStr(ctx, req, "uri", JS_NewString(ctx, uri ? uri : "/"));

    /* path — extract from REQUEST_URI (without query string) */
    const char *path = NULL;
    if (uri) {
        static char path_buf[4096];
        const char *qmark = strchr(uri, '?');
        if (qmark) {
            size_t plen = qmark - uri;
            memcpy(path_buf, uri, plen);
            path_buf[plen] = '\0';
            path = path_buf;
        } else {
            path = uri;
        }
    }
    JS_SetPropertyStr(ctx, req, "path", JS_NewString(ctx, path ? path : "/"));

    /* query object */
    JSValue query = JS_NewObject(ctx);
    const char *qs = getenv("QUERY_STRING");
    parse_query_string(ctx, query, qs);
    JS_SetPropertyStr(ctx, req, "query", query);

    /* headers */
    JSValue headers = JS_NewObject(ctx);
    parse_headers(ctx, headers);
    JS_SetPropertyStr(ctx, req, "headers", headers);

    /* body */
    char *body = read_request_body();
    JS_SetPropertyStr(ctx, req, "body", JS_NewString(ctx, body));
    free(body);

    /* cookies */
    JSValue cookies = JS_NewObject(ctx);
    const char *cookie_str = getenv("HTTP_COOKIE");
    if (cookie_str) {
        char *copy = strdup(cookie_str);
        char *pair = strtok(copy, ";");
        while (pair) {
            while (*pair == ' ') pair++;
            char *eq = strchr(pair, '=');
            if (eq) {
                *eq = '\0';
                JS_SetPropertyStr(ctx, cookies, pair, JS_NewString(ctx, eq + 1));
            }
            pair = strtok(NULL, ";");
        }
        free(copy);
    }
    JS_SetPropertyStr(ctx, req, "cookies", cookies);

    return req;
}

static JSValue js_response_set_cookie(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "setCookie requires a name and value");
    }

    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!name || !value) {
        if (name) JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        return JS_EXCEPTION;
    }

    char cookie[4096];
    int pos = snprintf(cookie, sizeof(cookie), "Set-Cookie: %s=%s; Path=/", name, value);

    /* Optional options object as 3rd argument */
    if (argc > 2 && JS_IsObject(argv[2])) {
        JSValue opt;

        opt = JS_GetPropertyStr(ctx, argv[2], "path");
        if (JS_IsString(opt)) {
            const char *p = JS_ToCString(ctx, opt);
            /* Overwrite the Path we already set */
            pos = snprintf(cookie, sizeof(cookie), "Set-Cookie: %s=%s; Path=%s", name, value, p);
            JS_FreeCString(ctx, p);
        }
        JS_FreeValue(ctx, opt);

        opt = JS_GetPropertyStr(ctx, argv[2], "maxAge");
        if (JS_IsNumber(opt)) {
            int64_t max_age;
            JS_ToInt64(ctx, &max_age, opt);
            pos += snprintf(cookie + pos, sizeof(cookie) - pos, "; Max-Age=%ld", (long)max_age);
        }
        JS_FreeValue(ctx, opt);

        opt = JS_GetPropertyStr(ctx, argv[2], "httpOnly");
        if (JS_ToBool(ctx, opt)) {
            pos += snprintf(cookie + pos, sizeof(cookie) - pos, "; HttpOnly");
        }
        JS_FreeValue(ctx, opt);

        opt = JS_GetPropertyStr(ctx, argv[2], "secure");
        if (JS_ToBool(ctx, opt)) {
            pos += snprintf(cookie + pos, sizeof(cookie) - pos, "; Secure");
        }
        JS_FreeValue(ctx, opt);

        opt = JS_GetPropertyStr(ctx, argv[2], "sameSite");
        if (JS_IsString(opt)) {
            const char *ss = JS_ToCString(ctx, opt);
            pos += snprintf(cookie + pos, sizeof(cookie) - pos, "; SameSite=%s", ss);
            JS_FreeCString(ctx, ss);
        }
        JS_FreeValue(ctx, opt);
    }

    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);

    response_append_header(cookie);

    return JS_UNDEFINED;
}

static JSValue build_response_object(JSContext *ctx) {
    JSValue resp = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, resp, "setHeader",
        JS_NewCFunction(ctx, js_response_set_header, "setHeader", 2));
    JS_SetPropertyStr(ctx, resp, "setStatus",
        JS_NewCFunction(ctx, js_response_set_status, "setStatus", 1));
    JS_SetPropertyStr(ctx, resp, "setCookie",
        JS_NewCFunction(ctx, js_response_set_cookie, "setCookie", 3));
    return resp;
}

/* -------------------- File reading -------------------- */

static char *read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    size_t read = fread(buf, 1, len, fp);
    buf[read] = '\0';
    fclose(fp);

    if (out_len) *out_len = len;
    return buf;
}

/* -------------------- Module loader -------------------- */

static char *module_normalize(JSContext *ctx, const char *module_base_name,
                              const char *module_name, void *opaque) {
    char tmp[8192];

    if (module_name[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", module_name);
    } else {
        char base_dir[4096];
        const char *slash = strrchr(module_base_name, '/');
        if (slash) {
            size_t len = slash - module_base_name;
            memcpy(base_dir, module_base_name, len);
            base_dir[len] = '\0';
        } else {
            strcpy(base_dir, g_script_dir);
        }
        snprintf(tmp, sizeof(tmp), "%s/%s", base_dir, module_name);
    }

    /* Resolve the path to remove ./ and ../ */
    char *resolved = realpath(tmp, NULL);
    if (resolved) {
        char *result = js_malloc(ctx, strlen(resolved) + 1);
        strcpy(result, resolved);
        free(resolved);
        return result;
    }

    /* realpath failed (file doesn't exist) — try to clean up the path manually */
    /* Remove /./ sequences */
    char cleaned[4096];
    char *src = tmp;
    char *dst = cleaned;
    while (*src) {
        if (src[0] == '/' && src[1] == '.' && src[2] == '/') {
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    char *result = js_malloc(ctx, strlen(cleaned) + 1);
    strcpy(result, cleaned);
    return result;
}

static JSModuleDef *module_loader(JSContext *ctx, const char *module_name, void *opaque) {
    size_t source_len;
    char *source = read_file(module_name, &source_len);

    if (!source) {
        JS_ThrowReferenceError(ctx, "Cannot load module '%s'", module_name);
        return NULL;
    }

    JSValue func_val = JS_Eval(ctx, source, source_len, module_name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(source);

    if (JS_IsException(func_val)) {
        return NULL;
    }

    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func_val);
    JS_FreeValue(ctx, func_val);
    return m;
}

/* -------------------- Extension loading -------------------- */

typedef struct {
    void *handle;
    jscgi_module_entry *entry;
} loaded_extension;

static loaded_extension g_extensions[MAX_EXTENSIONS];
static int g_extension_count = 0;

static int load_extensions(jscgi_config *cfg, JSContext *ctx, JSValue global) {
    for (int i = 0; i < cfg->extension_count; i++) {
        char path[4096];
        if (cfg->extensions[i][0] == '/') {
            snprintf(path, sizeof(path), "%s", cfg->extensions[i]);
        } else {
            snprintf(path, sizeof(path), "%s/%s", cfg->extension_dir, cfg->extensions[i]);
        }

        void *handle = dlopen(path, RTLD_NOW);
        if (!handle) {
            fprintf(stderr, "js-cgi: Failed to load extension '%s': %s\n", cfg->extensions[i], dlerror());
            continue;
        }

        jscgi_module_entry *(*get_module)(void) = dlsym(handle, "jscgi_get_module");
        if (!get_module) {
            fprintf(stderr, "js-cgi: Extension '%s' missing jscgi_get_module symbol\n", cfg->extensions[i]);
            dlclose(handle);
            continue;
        }

        jscgi_module_entry *entry = get_module();
        if (!entry) {
            fprintf(stderr, "js-cgi: Extension '%s' returned NULL module entry\n", cfg->extensions[i]);
            dlclose(handle);
            continue;
        }

        if (entry->module_init) {
            if (entry->module_init(ctx, global) != 0) {
                fprintf(stderr, "js-cgi: Extension '%s' init failed\n", entry->name);
                dlclose(handle);
                continue;
            }
        }

        g_extensions[g_extension_count].handle = handle;
        g_extensions[g_extension_count].entry = entry;
        g_extension_count++;
    }

    return 0;
}

static void unload_extensions(void) {
    for (int i = 0; i < g_extension_count; i++) {
        if (g_extensions[i].entry && g_extensions[i].entry->module_shutdown) {
            g_extensions[i].entry->module_shutdown();
        }
        if (g_extensions[i].handle) {
            dlclose(g_extensions[i].handle);
        }
    }
    g_extension_count = 0;
}

/* -------------------- Main -------------------- */

static jscgi_config *g_cfg = NULL;

static void log_error(const char *msg) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    if (g_cfg && g_cfg->error_log[0]) {
        FILE *fp = fopen(g_cfg->error_log, "a");
        if (fp) {
            fprintf(fp, "[%s] [js-cgi] %s\n", ts, msg);
            fclose(fp);
        }
    }
    fprintf(stderr, "[%s] [js-cgi] %s\n", ts, msg);
}

static void output_error(const char *msg) {
    log_error(msg);

    printf("Status: 500\r\n");
    printf("Content-Type: text/html\r\n");
    printf("\r\n");

    if (g_cfg && !g_cfg->display_errors) {
        printf("<!DOCTYPE html>\n<html>\n<head><title>500 Internal Server Error</title></head>\n<body>\n");
        printf("<h1>Internal Server Error</h1>\n");
        printf("<p>The server encountered an internal error and was unable to complete your request.</p>\n");
        printf("<hr>\n");
        printf("<address>js-cgi/" CONFIG_VERSION "</address>\n");
        printf("</body>\n</html>\n");
    } else {
        printf("<!DOCTYPE html>\n<html>\n<head><title>500 Internal Server Error</title></head>\n<body>\n");
        printf("<h1>Internal Server Error</h1>\n");
        printf("<pre>%s</pre>\n", msg);
        printf("<hr>\n");
        printf("<address>js-cgi/" CONFIG_VERSION "</address>\n");
        printf("</body>\n</html>\n");
    }
}

/* -------------------- Development server -------------------- */

static const char *mime_type_for(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) return "text/html";
    if (strcasecmp(ext, "css") == 0) return "text/css";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, "json") == 0) return "application/json";
    if (strcasecmp(ext, "txt") == 0) return "text/plain";
    if (strcasecmp(ext, "woff") == 0) return "font/woff";
    if (strcasecmp(ext, "woff2") == 0) return "font/woff2";
    if (strcasecmp(ext, "ttf") == 0) return "font/ttf";
    if (strcasecmp(ext, "xml") == 0) return "application/xml";
    if (strcasecmp(ext, "pdf") == 0) return "application/pdf";
    if (strcasecmp(ext, "wasm") == 0) return "application/wasm";
    return "application/octet-stream";
}

static void serve_static(int client_fd, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        dprintf(client_fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                "<h1>404 Not Found</h1>\n");
        return;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    const char *mime = mime_type_for(path);
    dprintf(client_fd, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n", mime, size);

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        (void)write(client_fd, buf, n);
    }
    fclose(fp);
}

static void handle_request(int client_fd, const char *doc_root, const char *ini_override) {
    char request_buf[65536];
    ssize_t total = 0;
    ssize_t n;

    /* Read the request headers */
    while (total < (ssize_t)sizeof(request_buf) - 1) {
        n = read(client_fd, request_buf + total, sizeof(request_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        request_buf[total] = '\0';
        if (strstr(request_buf, "\r\n\r\n")) break;
    }

    if (total <= 0) return;

    /* Parse request line */
    char method[16] = {0};
    char uri[4096] = {0};
    sscanf(request_buf, "%15s %4095s", method, uri);

    /* Separate path and query string */
    char path[4096];
    char query_string[4096] = {0};
    char *qmark = strchr(uri, '?');
    if (qmark) {
        size_t plen = qmark - uri;
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, uri, plen);
        path[plen] = '\0';
        snprintf(query_string, sizeof(query_string), "%s", qmark + 1);
    } else {
        snprintf(path, sizeof(path), "%s", uri);
    }

    /* Resolve file path */
    char file_path[8192];
    snprintf(file_path, sizeof(file_path), "%s%s", doc_root, path);

    /* Directory index */
    struct stat st;
    if (stat(file_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t len = strlen(file_path);
        if (len > 0 && file_path[len - 1] != '/') {
            file_path[len] = '/';
            file_path[len + 1] = '\0';
        }
        strncat(file_path, "index.js", sizeof(file_path) - strlen(file_path) - 1);
        if (stat(file_path, &st) != 0) {
            /* Try index.html */
            file_path[strlen(file_path) - 8] = '\0';
            strncat(file_path, "index.html", sizeof(file_path) - strlen(file_path) - 1);
        }
    }

    /* Check if file exists */
    if (stat(file_path, &st) != 0) {
        dprintf(client_fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                "<h1>404 Not Found</h1><p>%s</p>\n", path);
        fprintf(stderr, "[%s] 404 %s %s\n", method, path, "Not Found");
        return;
    }

    /* If not a .js file, serve static */
    const char *ext = strrchr(file_path, '.');
    if (!ext || strcasecmp(ext, ".js") != 0) {
        serve_static(client_fd, file_path);
        fprintf(stderr, "[%s] 200 %s (static)\n", method, path);
        return;
    }

    /* Parse headers for CGI env */
    char content_type[256] = {0};
    char content_length[32] = {0};
    char cookie[4096] = {0};
    char *headers_start = strstr(request_buf, "\r\n");
    if (headers_start) {
        headers_start += 2;
        char *line = headers_start;
        while (line && *line && !(line[0] == '\r' && line[1] == '\n')) {
            char *eol = strstr(line, "\r\n");
            if (!eol) break;
            *eol = '\0';

            if (strncasecmp(line, "Content-Type:", 13) == 0) {
                char *val = line + 13;
                while (*val == ' ') val++;
                strncpy(content_type, val, sizeof(content_type) - 1);
            } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
                char *val = line + 15;
                while (*val == ' ') val++;
                strncpy(content_length, val, sizeof(content_length) - 1);
            } else if (strncasecmp(line, "Cookie:", 7) == 0) {
                char *val = line + 7;
                while (*val == ' ') val++;
                strncpy(cookie, val, sizeof(cookie) - 1);
            }

            *eol = '\r';
            line = eol + 2;
        }
    }

    /* Read request body if present */
    char *body_start = strstr(request_buf, "\r\n\r\n");
    char *body_data = NULL;
    size_t body_len = 0;
    if (body_start) {
        body_start += 4;
        body_len = total - (body_start - request_buf);
        int cl = content_length[0] ? atoi(content_length) : 0;
        if (cl > (int)body_len) {
            body_data = malloc(cl + 1);
            memcpy(body_data, body_start, body_len);
            while ((int)body_len < cl) {
                n = read(client_fd, body_data + body_len, cl - body_len);
                if (n <= 0) break;
                body_len += n;
            }
            body_data[body_len] = '\0';
        } else if (body_len > 0) {
            body_data = malloc(body_len + 1);
            memcpy(body_data, body_start, body_len);
            body_data[body_len] = '\0';
        }
    }

    /* Fork and run js-cgi engine via CGI environment */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        dprintf(client_fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n");
        free(body_data);
        return;
    }

    /* Pipe for request body (stdin of child) */
    int body_pipe[2] = {-1, -1};
    if (body_data && body_len > 0) {
        if (pipe(body_pipe) < 0) {
            dprintf(client_fd, "HTTP/1.1 500 Internal Server Error\r\n\r\n");
            free(body_data);
            close(pipefd[0]);
            close(pipefd[1]);
            return;
        }
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: set up CGI env and run engine */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        if (body_pipe[0] >= 0) {
            close(body_pipe[1]);
            dup2(body_pipe[0], STDIN_FILENO);
            close(body_pipe[0]);
        }

        setenv("REQUEST_METHOD", method, 1);
        setenv("REQUEST_URI", uri, 1);
        setenv("QUERY_STRING", query_string, 1);
        setenv("PATH_TRANSLATED", file_path, 1);
        setenv("SCRIPT_FILENAME", file_path, 1);
        setenv("DOCUMENT_ROOT", doc_root, 1);
        if (content_type[0]) setenv("CONTENT_TYPE", content_type, 1);
        if (content_length[0]) setenv("CONTENT_LENGTH", content_length, 1);
        if (cookie[0]) setenv("HTTP_COOKIE", cookie, 1);

        /* Pass all HTTP_* headers */
        if (headers_start) {
            char *line = headers_start;
            while (line && *line && !(line[0] == '\r' && line[1] == '\n')) {
                char *eol = strstr(line, "\r\n");
                if (!eol) break;
                *eol = '\0';

                char *colon = strchr(line, ':');
                if (colon) {
                    *colon = '\0';
                    char *val = colon + 1;
                    while (*val == ' ') val++;

                    /* Convert header name to HTTP_UPPER_CASE */
                    char env_name[256] = "HTTP_";
                    size_t i;
                    for (i = 0; i < strlen(line) && i < 245; i++) {
                        char c = line[i];
                        if (c == '-') env_name[5 + i] = '_';
                        else if (c >= 'a' && c <= 'z') env_name[5 + i] = c - 32;
                        else env_name[5 + i] = c;
                    }
                    env_name[5 + i] = '\0';

                    setenv(env_name, val, 1);
                    *colon = ':';
                }

                *eol = '\r';
                line = eol + 2;
            }
        }

        /* Re-exec ourselves to run the script */
        char *args[4];
        args[0] = (char *)"js-cgi";
        int ai = 1;
        char ini_arg[4112];
        if (ini_override) {
            snprintf(ini_arg, sizeof(ini_arg), "--ini=%s", ini_override);
            args[ai++] = ini_arg;
        }
        args[ai++] = (char *)file_path;
        args[ai] = NULL;

        execv("/proc/self/exe", args);
        _exit(1);
    }

    /* Parent */
    close(pipefd[1]);

    /* Write body to child's stdin */
    if (body_pipe[1] >= 0) {
        close(body_pipe[0]);
        if (body_data && body_len > 0) {
            (void)write(body_pipe[1], body_data, body_len);
        }
        close(body_pipe[1]);
    }
    free(body_data);

    /* Read CGI response from child */
    char response_buf[1048576];
    size_t resp_total = 0;
    while ((n = read(pipefd[0], response_buf + resp_total, sizeof(response_buf) - 1 - resp_total)) > 0) {
        resp_total += n;
    }
    response_buf[resp_total] = '\0';
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    /* Parse CGI output: convert Status header to HTTP status line */
    char *resp_headers_end = strstr(response_buf, "\r\n\r\n");
    if (!resp_headers_end) {
        dprintf(client_fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\n\r\n"
                "<h1>500 Internal Server Error</h1><p>Malformed CGI response</p>\n");
        fprintf(stderr, "[%s] 500 %s\n", method, path);
        return;
    }

    /* Extract status code from CGI headers */
    int http_status = 200;
    char *status_line = strstr(response_buf, "Status: ");
    if (status_line && status_line < resp_headers_end) {
        http_status = atoi(status_line + 8);
    }

    /* Build HTTP response */
    const char *reason = "OK";
    if (http_status == 201) reason = "Created";
    else if (http_status == 204) reason = "No Content";
    else if (http_status == 301) reason = "Moved Permanently";
    else if (http_status == 302) reason = "Found";
    else if (http_status == 304) reason = "Not Modified";
    else if (http_status == 400) reason = "Bad Request";
    else if (http_status == 401) reason = "Unauthorized";
    else if (http_status == 403) reason = "Forbidden";
    else if (http_status == 404) reason = "Not Found";
    else if (http_status == 405) reason = "Method Not Allowed";
    else if (http_status == 500) reason = "Internal Server Error";
    dprintf(client_fd, "HTTP/1.1 %d %s\r\n", http_status, reason);

    /* Write headers, skipping the Status: line */
    char *hdr = response_buf;
    while (hdr < resp_headers_end) {
        char *eol = strstr(hdr, "\r\n");
        if (!eol) break;
        if (strncmp(hdr, "Status:", 7) != 0) {
            (void)write(client_fd, hdr, eol - hdr + 2);
        }
        hdr = eol + 2;
    }

    /* End of headers + body */
    (void)write(client_fd, "\r\n", 2);
    char *body_out = resp_headers_end + 4;
    size_t body_out_len = resp_total - (body_out - response_buf);
    if (body_out_len > 0) {
        (void)write(client_fd, body_out, body_out_len);
    }

    fprintf(stderr, "[%s] %d %s\n", method, http_status, path);
}

static void reap_children(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

static int run_dev_server(const char *host, int port, const char *doc_root, const char *ini_override) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "Error: cannot create socket\n");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: cannot bind to %s:%d — %s\n", host, port, strerror(errno));
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 64) < 0) {
        fprintf(stderr, "Error: listen failed — %s\n", strerror(errno));
        close(server_fd);
        return 1;
    }

    /* Reap zombie child processes */
    struct sigaction sa;
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    fprintf(stderr, "js-cgi development server\n");
    fprintf(stderr, "Listening on http://%s:%d\n", host, port);
    fprintf(stderr, "Document root: %s\n", doc_root);
    fprintf(stderr, "Press Ctrl+C to stop.\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        pid_t pid = fork();
        if (pid == 0) {
            close(server_fd);
            handle_request(client_fd, doc_root, ini_override);
            close(client_fd);
            _exit(0);
        }
        close(client_fd);
    }

    close(server_fd);
    return 0;
}

/* -------------------- Main -------------------- */

int main(int argc, char **argv) {
    const char *ini_override = NULL;
    const char *script_path = NULL;
    const char *serve_arg = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--ini=", 6) == 0) {
            ini_override = argv[i] + 6;
        } else if (strcmp(argv[i], "--serve") == 0) {
            serve_arg = (i + 1 < argc) ? argv[++i] : "8000";
        } else {
            script_path = argv[i];
        }
    }

    /* Dev server mode */
    if (serve_arg) {
        char host[256] = "localhost";
        int port = 8000;

        /* Parse host:port or just port */
        const char *colon = strchr(serve_arg, ':');
        if (colon) {
            size_t hlen = colon - serve_arg;
            if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
            memcpy(host, serve_arg, hlen);
            host[hlen] = '\0';
            port = atoi(colon + 1);
        } else {
            port = atoi(serve_arg);
        }

        /* Document root defaults to cwd */
        char doc_root[4096];
        if (script_path) {
            char *rp = realpath(script_path, doc_root);
            if (!rp) {
                fprintf(stderr, "Error: invalid document root '%s'\n", script_path);
                return 1;
            }
        } else {
            if (!getcwd(doc_root, sizeof(doc_root))) {
                fprintf(stderr, "Error: cannot determine working directory\n");
                return 1;
            }
        }

        return run_dev_server(host, port, doc_root, ini_override);
    }

    /* In CGI mode, script path comes from environment.
       With Apache Action directive, PATH_TRANSLATED holds the actual file. */
    if (!script_path) {
        script_path = getenv("PATH_TRANSLATED");
        if (!script_path) {
            script_path = getenv("SCRIPT_FILENAME");
        }
    }

    if (!script_path) {
        output_error("No script specified");
        return 1;
    }

    /* Load configuration */
    jscgi_config cfg;
    config_load(&cfg, ini_override);
    g_cfg = &cfg;

    /* Read the script */
    size_t script_len;
    char *script = read_file(script_path, &script_len);
    if (!script) {
        output_error("Cannot read script file");
        return 1;
    }

    /* Initialize response */
    response_init();

    /* Create QuickJS runtime */
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMemoryLimit(rt, cfg.memory_limit);
    JS_SetMaxStackSize(rt, 1024 * 1024);

    /* Set up interrupt handler for time limit */
    g_start_time = get_time_ms();
    g_max_execution_time = cfg.max_execution_time;
    JS_SetInterruptHandler(rt, interrupt_handler, NULL);

    /* Create context */
    JSContext *ctx = JS_NewContext(rt);

    /* Set up globals */
    JSValue global = JS_GetGlobalObject(ctx);

    /* print() */
    JS_SetPropertyStr(ctx, global, "print",
        JS_NewCFunction(ctx, js_print, "print", 1));

    /* include() */
    JS_SetPropertyStr(ctx, global, "include",
        JS_NewCFunction(ctx, js_include, "include", 1));

    /* request */
    JS_SetPropertyStr(ctx, global, "request", build_request_object(ctx));

    /* response */
    JS_SetPropertyStr(ctx, global, "response", build_response_object(ctx));

    /* Load extensions */
    load_extensions(&cfg, ctx, global);

    JS_FreeValue(ctx, global);

    /* Determine script directory for module resolution */
    const char *last_slash = strrchr(script_path, '/');
    if (last_slash) {
        size_t dir_len = last_slash - script_path;
        memcpy(g_script_dir, script_path, dir_len);
        g_script_dir[dir_len] = '\0';
    } else {
        strcpy(g_script_dir, ".");
    }

    /* Register module loader */
    JS_SetModuleLoaderFunc(rt, module_normalize, module_loader, NULL);

    /* Detect module syntax */
    int eval_flags = JS_EVAL_TYPE_GLOBAL;
    if (strstr(script, "import ") || strstr(script, "export ")) {
        eval_flags = JS_EVAL_TYPE_MODULE;
    }

    /* Execute script */
    const char *filename = last_slash ? last_slash + 1 : script_path;

    JSValue result = JS_Eval(ctx, script, script_len, filename, eval_flags);

    /* Drain the job queue (resolves dynamic import() and other promises) */
    if (!JS_IsException(result)) {
        JSContext *ctx1;
        int ret;
        while ((ret = JS_ExecutePendingJob(rt, &ctx1)) > 0) {}
        if (ret < 0) {
            JS_FreeValue(ctx, result);
            result = JS_EXCEPTION;
        }
    }

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);

        /* Check for timeout */
        uint64_t elapsed = get_time_ms() - g_start_time;
        if (elapsed >= (uint64_t)g_max_execution_time * 1000) {
            output_error("Maximum execution time exceeded");
        } else {
            char err_buf[4096];
            const char *stack = NULL;
            if (JS_IsObject(exc)) {
                JSValue stack_val = JS_GetPropertyStr(ctx, exc, "stack");
                if (JS_IsString(stack_val)) {
                    stack = JS_ToCString(ctx, stack_val);
                }
                JS_FreeValue(ctx, stack_val);
            }

            if (msg && stack) {
                snprintf(err_buf, sizeof(err_buf), "%s\n%s", msg, stack);
            } else if (msg) {
                snprintf(err_buf, sizeof(err_buf), "%s", msg);
            } else {
                snprintf(err_buf, sizeof(err_buf), "Unknown error");
            }

            output_error(err_buf);
            if (stack) JS_FreeCString(ctx, stack);
        }

        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    } else {
        /* Output the response */
        response_output();
    }

    JS_FreeValue(ctx, result);
    unload_extensions();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    response_free();
    free(script);

    for (int i = 0; i < cfg.extension_count; i++) {
        free(cfg.extensions[i]);
    }

    return 0;
}
