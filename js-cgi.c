#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>

#include "quickjs/quickjs.h"
#include "js-cgi-module.h"

/* Forward declarations */
char g_script_dir[4096];
static char *read_file(const char *path, size_t *out_len);

#define JSCGI_VERSION "0.1.2"
#define DEFAULT_MEMORY_LIMIT (128 * 1024 * 1024)
#define DEFAULT_MAX_EXECUTION_TIME 30
#define DEFAULT_CONTENT_TYPE "text/html"
#define INI_SYSTEM_PATH "/etc/js-cgi/js-cgi.ini"
#define INI_MAX_LINE 1024
#define MAX_EXTENSIONS 64
#define MAX_TEMP_FILES 64

static char *g_temp_files[MAX_TEMP_FILES];
static int g_temp_file_count = 0;

static void track_temp_file(const char *path) {
    if (g_temp_file_count < MAX_TEMP_FILES) {
        g_temp_files[g_temp_file_count++] = strdup(path);
    }
}

static void cleanup_temp_files(void) {
    for (int i = 0; i < g_temp_file_count; i++) {
        unlink(g_temp_files[i]);
        free(g_temp_files[i]);
    }
    g_temp_file_count = 0;
}

/* -------------------- Configuration -------------------- */

typedef struct {
    size_t memory_limit;
    int max_execution_time;
    char extension_dir[4096];
    char *extensions[MAX_EXTENSIONS];
    int extension_count;
    int display_errors;
    char error_log[4096];
    size_t upload_max_filesize;
    char upload_dir[4096];
} jscgi_config;

static void config_init(jscgi_config *cfg) {
    cfg->memory_limit = DEFAULT_MEMORY_LIMIT;
    cfg->max_execution_time = DEFAULT_MAX_EXECUTION_TIME;
    strcpy(cfg->extension_dir, "/usr/lib/js-cgi/modules");
    cfg->extension_count = 0;
    cfg->display_errors = 1;
    cfg->error_log[0] = '\0';
    cfg->upload_max_filesize = 2 * 1024 * 1024;
    strcpy(cfg->upload_dir, "/tmp");
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
        } else if (strcmp(key, "upload_max_filesize") == 0) {
            cfg->upload_max_filesize = parse_memory_value(val);
        } else if (strcmp(key, "upload_dir") == 0) {
            strncpy(cfg->upload_dir, val, sizeof(cfg->upload_dir) - 1);
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

    /* Content-Length */
    printf("Content-Length: %zu\r\n", g_response.body_len);

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

/* -------------------- JS API: console -------------------- */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        if (i > 0) fprintf(stderr, " ");
        const char *str = JS_ToCString(ctx, argv[i]);
        if (str) {
            fprintf(stderr, "%s", str);
            JS_FreeCString(ctx, str);
        }
    }
    fprintf(stderr, "\n");
    return JS_UNDEFINED;
}

/* -------------------- JS API: move() -------------------- */

static JSValue js_move(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "move requires a source and destination path");
    }

    const char *src = JS_ToCString(ctx, argv[0]);
    const char *dst = JS_ToCString(ctx, argv[1]);
    if (!src || !dst) {
        if (src) JS_FreeCString(ctx, src);
        if (dst) JS_FreeCString(ctx, dst);
        return JS_EXCEPTION;
    }

    int result = rename(src, dst);
    JS_FreeCString(ctx, src);
    JS_FreeCString(ctx, dst);

    if (result != 0) {
        return JS_ThrowTypeError(ctx, "move failed: %s", strerror(errno));
    }

    return JS_TRUE;
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

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *str) {
    char *src = str;
    char *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && hex_digit(src[1]) >= 0 && hex_digit(src[2]) >= 0) {
            *dst++ = (char)(hex_digit(src[1]) << 4 | hex_digit(src[2]));
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void parse_query_string(JSContext *ctx, JSValue obj, const char *qs) {
    if (!qs || !*qs) return;

    char *copy = strdup(qs);
    char *pair = strtok(copy, "&");

    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            url_decode(pair);
            url_decode(eq + 1);
            JS_SetPropertyStr(ctx, obj, pair, JS_NewString(ctx, eq + 1));
        } else {
            url_decode(pair);
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

static JSValue build_request_object(JSContext *ctx, jscgi_config *cfg) {
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
    const char *ct = getenv("CONTENT_TYPE");

    if (ct && strncmp(ct, "multipart/form-data", 19) == 0) {
        /* Parse multipart form data */
        const char *boundary_start = strstr(ct, "boundary=");
        if (boundary_start) {
            boundary_start += 9;
            char boundary[256];
            snprintf(boundary, sizeof(boundary), "--%s", boundary_start);
            size_t boundary_len = strlen(boundary);

            JSValue fields = JS_NewObject(ctx);
            JSValue files = JS_NewObject(ctx);

            const char *cl_str = getenv("CONTENT_LENGTH");
            size_t body_len = cl_str ? atoi(cl_str) : strlen(body);
            const char *pos = body;
            const char *end = body + body_len;

            while (pos < end) {
                /* Find boundary */
                const char *part_start = memmem(pos, end - pos, boundary, boundary_len);
                if (!part_start) break;
                part_start += boundary_len;

                /* Check for final boundary (--) */
                if (part_start[0] == '-' && part_start[1] == '-') break;

                /* Skip CRLF after boundary */
                if (part_start[0] == '\r') part_start++;
                if (part_start[0] == '\n') part_start++;

                /* Find end of this part */
                const char *part_end = memmem(part_start, end - part_start, boundary, boundary_len);
                if (!part_end) break;

                /* Remove trailing CRLF before next boundary */
                const char *data_end = part_end;
                if (data_end > part_start && data_end[-1] == '\n') data_end--;
                if (data_end > part_start && data_end[-1] == '\r') data_end--;

                /* Parse part headers */
                const char *headers_end = memmem(part_start, data_end - part_start, "\r\n\r\n", 4);
                if (!headers_end) { pos = part_end; continue; }

                size_t hdr_len = headers_end - part_start;
                const char *data_start = headers_end + 4;

                /* Extract name and filename from Content-Disposition */
                char field_name[256] = {0};
                char file_name[256] = {0};
                char part_ct[256] = {0};

                /* Search headers for Content-Disposition and Content-Type */
                const char *h = part_start;
                while (h < headers_end) {
                    const char *line_end = memmem(h, headers_end - h, "\r\n", 2);
                    if (!line_end) line_end = headers_end;

                    if (strncasecmp(h, "Content-Disposition:", 20) == 0) {
                        const char *name_pos = strstr(h, "name=\"");
                        if (name_pos && name_pos < line_end) {
                            name_pos += 6;
                            const char *name_end = strchr(name_pos, '"');
                            if (name_end && name_end < line_end) {
                                size_t nlen = name_end - name_pos;
                                if (nlen >= sizeof(field_name)) nlen = sizeof(field_name) - 1;
                                memcpy(field_name, name_pos, nlen);
                                field_name[nlen] = '\0';
                            }
                        }
                        const char *fn_pos = strstr(h, "filename=\"");
                        if (fn_pos && fn_pos < line_end) {
                            fn_pos += 10;
                            const char *fn_end = strchr(fn_pos, '"');
                            if (fn_end && fn_end < line_end) {
                                size_t fnlen = fn_end - fn_pos;
                                if (fnlen >= sizeof(file_name)) fnlen = sizeof(file_name) - 1;
                                memcpy(file_name, fn_pos, fnlen);
                                file_name[fnlen] = '\0';
                            }
                        }
                    } else if (strncasecmp(h, "Content-Type:", 13) == 0) {
                        const char *ct_val = h + 13;
                        while (*ct_val == ' ') ct_val++;
                        size_t ct_len = line_end - ct_val;
                        if (ct_len >= sizeof(part_ct)) ct_len = sizeof(part_ct) - 1;
                        memcpy(part_ct, ct_val, ct_len);
                        part_ct[ct_len] = '\0';
                    }

                    h = line_end + 2;
                }

                size_t data_len = data_end - data_start;

                if (field_name[0]) {
                    if (file_name[0]) {
                        /* File upload */
                        if (data_len > cfg->upload_max_filesize) {
                            JSValue file_obj = JS_NewObject(ctx);
                            JS_SetPropertyStr(ctx, file_obj, "filename", JS_NewString(ctx, file_name));
                            JS_SetPropertyStr(ctx, file_obj, "error", JS_NewString(ctx, "File exceeds upload_max_filesize"));
                            JS_SetPropertyStr(ctx, file_obj, "size", JS_NewInt64(ctx, data_len));
                            JS_SetPropertyStr(ctx, files, field_name, file_obj);
                        } else {
                            /* Generate random temp filename */
                            unsigned char rand_bytes[8];
                            FILE *urand = fopen("/dev/urandom", "rb");
                            if (urand) {
                                fread(rand_bytes, 1, 8, urand);
                                fclose(urand);
                            } else {
                                for (int r = 0; r < 8; r++) rand_bytes[r] = (unsigned char)(getpid() + r * 37);
                            }
                            char rand_hex[17];
                            for (int r = 0; r < 8; r++) snprintf(rand_hex + r * 2, 3, "%02x", rand_bytes[r]);

                            char tmp_path[4352];
                            snprintf(tmp_path, sizeof(tmp_path), "%s/jscgi_%s", cfg->upload_dir, rand_hex);
                            FILE *tmp_fp = fopen(tmp_path, "wb");
                            if (tmp_fp) {
                                fwrite(data_start, 1, data_len, tmp_fp);
                                fclose(tmp_fp);
                                track_temp_file(tmp_path);
                                JSValue file_obj = JS_NewObject(ctx);
                                JS_SetPropertyStr(ctx, file_obj, "filename", JS_NewString(ctx, file_name));
                                JS_SetPropertyStr(ctx, file_obj, "contentType", JS_NewString(ctx, part_ct[0] ? part_ct : "application/octet-stream"));
                                JS_SetPropertyStr(ctx, file_obj, "tmpPath", JS_NewString(ctx, tmp_path));
                                JS_SetPropertyStr(ctx, file_obj, "size", JS_NewInt64(ctx, data_len));
                                JS_SetPropertyStr(ctx, files, field_name, file_obj);
                            } else {
                                JSValue file_obj = JS_NewObject(ctx);
                                JS_SetPropertyStr(ctx, file_obj, "filename", JS_NewString(ctx, file_name));
                                JS_SetPropertyStr(ctx, file_obj, "error", JS_NewString(ctx, "Failed to write temp file"));
                                JS_SetPropertyStr(ctx, files, field_name, file_obj);
                            }
                        }
                    } else {
                        /* Regular field */
                        JS_SetPropertyStr(ctx, fields, field_name, JS_NewStringLen(ctx, data_start, data_len));
                    }
                }

                pos = part_end;
            }

            JS_SetPropertyStr(ctx, req, "body", fields);
            JS_SetPropertyStr(ctx, req, "files", files);
        } else {
            JS_SetPropertyStr(ctx, req, "body", JS_NewString(ctx, body));
            JS_SetPropertyStr(ctx, req, "files", JS_NewObject(ctx));
        }
    } else if (ct && strncmp(ct, "application/x-www-form-urlencoded", 33) == 0) {
        /* Parse URL-encoded form body into an object */
        JSValue fields = JS_NewObject(ctx);
        parse_query_string(ctx, fields, body);
        JS_SetPropertyStr(ctx, req, "body", fields);
        JS_SetPropertyStr(ctx, req, "files", JS_NewObject(ctx));
    } else {
        JS_SetPropertyStr(ctx, req, "body", JS_NewString(ctx, body));
        JS_SetPropertyStr(ctx, req, "files", JS_NewObject(ctx));
    }
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
                url_decode(eq + 1);
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
        printf("<address>js-cgi/" JSCGI_VERSION "</address>\n");
        printf("</body>\n</html>\n");
    } else {
        printf("<!DOCTYPE html>\n<html>\n<head><title>500 Internal Server Error</title></head>\n<body>\n");
        printf("<h1>Internal Server Error</h1>\n");
        printf("<pre>%s</pre>\n", msg);
        printf("<hr>\n");
        printf("<address>js-cgi/" JSCGI_VERSION "</address>\n");
        printf("</body>\n</html>\n");
    }
}

/* -------------------- Script execution engine -------------------- */

typedef struct {
    const char *method;
    const char *uri;
    const char *query_string;
    const char *content_type;
    const char *content_length;
    const char *cookie;
    const char *script_filename;
    const char *body;
    size_t body_len;
    /* Full params for header passthrough */
    char **params;
    int param_count;
} cgi_request;

static int execute_script(jscgi_config *cfg, const char *script_path, cgi_request *req, int script_argc, char **script_argv) {
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
    JS_SetMemoryLimit(rt, cfg->memory_limit);
    JS_SetMaxStackSize(rt, 1024 * 1024);

    /* Set up interrupt handler for time limit */
    g_start_time = get_time_ms();
    g_max_execution_time = cfg->max_execution_time;
    JS_SetInterruptHandler(rt, interrupt_handler, NULL);

    /* Create context */
    JSContext *ctx = JS_NewContext(rt);

    /* Set up CGI env vars if we have a request struct */
    if (req) {
        if (req->method) setenv("REQUEST_METHOD", req->method, 1);
        if (req->uri) setenv("REQUEST_URI", req->uri, 1);
        if (req->query_string) setenv("QUERY_STRING", req->query_string, 1);
        if (req->content_type) setenv("CONTENT_TYPE", req->content_type, 1);
        if (req->content_length) setenv("CONTENT_LENGTH", req->content_length, 1);
        if (req->cookie) setenv("HTTP_COOKIE", req->cookie, 1);
        setenv("PATH_TRANSLATED", script_path, 1);
        setenv("SCRIPT_FILENAME", script_path, 1);

        /* Set all params as env vars */
        for (int i = 0; i < req->param_count; i += 2) {
            setenv(req->params[i], req->params[i + 1], 1);
        }
    }

    /* Set up globals */
    JSValue global = JS_GetGlobalObject(ctx);

    JS_SetPropertyStr(ctx, global, "print",
        JS_NewCFunction(ctx, js_print, "print", 1));
    JS_SetPropertyStr(ctx, global, "include",
        JS_NewCFunction(ctx, js_include, "include", 1));
    JS_SetPropertyStr(ctx, global, "move",
        JS_NewCFunction(ctx, js_move, "move", 2));

    JSValue console_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console_obj, "log",
        JS_NewCFunction(ctx, js_console_log, "log", 1));
    JS_SetPropertyStr(ctx, console_obj, "error",
        JS_NewCFunction(ctx, js_console_log, "error", 1));
    JS_SetPropertyStr(ctx, console_obj, "warn",
        JS_NewCFunction(ctx, js_console_log, "warn", 1));
    JS_SetPropertyStr(ctx, console_obj, "info",
        JS_NewCFunction(ctx, js_console_log, "info", 1));
    JS_SetPropertyStr(ctx, global, "console", console_obj);

    JS_SetPropertyStr(ctx, global, "request", build_request_object(ctx, cfg));
    JS_SetPropertyStr(ctx, global, "response", build_response_object(ctx));

    /* Expose script arguments as global argv array */
    JSValue argv_array = JS_NewArray(ctx);
    for (int i = 0; i < script_argc; i++) {
        JS_SetPropertyUint32(ctx, argv_array, i, JS_NewString(ctx, script_argv[i]));
    }
    JS_SetPropertyStr(ctx, global, "argv", argv_array);

    load_extensions(cfg, ctx, global);
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

    JS_SetModuleLoaderFunc(rt, module_normalize, module_loader, NULL);

    /* Detect module syntax — only match import/export at the start of a line */
    int eval_flags = JS_EVAL_TYPE_GLOBAL;
    const char *p = script;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if ((strncmp(p, "import ", 7) == 0 || strncmp(p, "import{", 7) == 0 ||
             strncmp(p, "export ", 7) == 0 || strncmp(p, "export{", 7) == 0 ||
             strncmp(p, "export;", 7) == 0)) {
            eval_flags = JS_EVAL_TYPE_MODULE;
            break;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }

    /* Execute script */
    const char *filename = last_slash ? last_slash + 1 : script_path;
    JSValue result = JS_Eval(ctx, script, script_len, filename, eval_flags);

    /* Drain the job queue */
    if (!JS_IsException(result)) {
        JSContext *ctx1;
        int ret;
        while ((ret = JS_ExecutePendingJob(rt, &ctx1)) > 0) {}
        if (ret < 0) {
            JS_FreeValue(ctx, result);
            result = JS_EXCEPTION;
        }
    }

    int success = 1;
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);

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
        success = 0;
    } else {
        response_output();
    }

    JS_FreeValue(ctx, result);
    unload_extensions();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    response_free();
    cleanup_temp_files();
    free(script);

    return success ? 0 : 1;
}

/* -------------------- FastCGI protocol -------------------- */

#define FCGI_VERSION_1       1
#define FCGI_BEGIN_REQUEST   1
#define FCGI_ABORT_REQUEST   2
#define FCGI_END_REQUEST     3
#define FCGI_PARAMS          4
#define FCGI_STDIN           5
#define FCGI_STDOUT          6
#define FCGI_STDERR          7

#define FCGI_RESPONDER       1
#define FCGI_REQUEST_COMPLETE 0

typedef struct {
    unsigned char version;
    unsigned char type;
    unsigned char requestIdB1;
    unsigned char requestIdB0;
    unsigned char contentLengthB1;
    unsigned char contentLengthB0;
    unsigned char paddingLength;
    unsigned char reserved;
} fcgi_header;

static int fcgi_read_exact(int fd, void *buf, size_t count) {
    size_t total = 0;
    while (total < count) {
        ssize_t n = read(fd, (char *)buf + total, count - total);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int fcgi_write_record(int fd, int type, int request_id,
                             const char *data, size_t len) {
    while (len > 0 || type == FCGI_STDOUT) {
        size_t chunk = len > 65535 ? 65535 : len;
        fcgi_header hdr;
        hdr.version = FCGI_VERSION_1;
        hdr.type = type;
        hdr.requestIdB1 = (request_id >> 8) & 0xFF;
        hdr.requestIdB0 = request_id & 0xFF;
        hdr.contentLengthB1 = (chunk >> 8) & 0xFF;
        hdr.contentLengthB0 = chunk & 0xFF;
        hdr.paddingLength = 0;
        hdr.reserved = 0;

        if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
        if (chunk > 0) {
            if (write(fd, data, chunk) != (ssize_t)chunk) return -1;
            data += chunk;
            len -= chunk;
        }
        if (type == FCGI_STDOUT && chunk == 0) break;
        if (len == 0 && type != FCGI_STDOUT) break;
    }
    return 0;
}

static int fcgi_write_end_request(int fd, int request_id, int app_status) {
    fcgi_header hdr;
    hdr.version = FCGI_VERSION_1;
    hdr.type = FCGI_END_REQUEST;
    hdr.requestIdB1 = (request_id >> 8) & 0xFF;
    hdr.requestIdB0 = request_id & 0xFF;
    hdr.contentLengthB1 = 0;
    hdr.contentLengthB0 = 8;
    hdr.paddingLength = 0;
    hdr.reserved = 0;

    unsigned char body[8] = {0};
    body[0] = (app_status >> 24) & 0xFF;
    body[1] = (app_status >> 16) & 0xFF;
    body[2] = (app_status >> 8) & 0xFF;
    body[3] = app_status & 0xFF;
    body[4] = FCGI_REQUEST_COMPLETE;

    if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
    if (write(fd, body, 8) != 8) return -1;
    return 0;
}

static int fcgi_decode_name_value(const char *buf, size_t buf_len,
                                  char **params, int *param_count, int max_params) {
    size_t pos = 0;
    while (pos < buf_len && *param_count < max_params - 1) {
        uint32_t name_len, value_len;

        if (pos >= buf_len) break;
        if ((unsigned char)buf[pos] >> 7) {
            if (pos + 4 > buf_len) break;
            name_len = ((unsigned char)buf[pos] & 0x7F) << 24 |
                       (unsigned char)buf[pos+1] << 16 |
                       (unsigned char)buf[pos+2] << 8 |
                       (unsigned char)buf[pos+3];
            pos += 4;
        } else {
            name_len = (unsigned char)buf[pos];
            pos += 1;
        }

        if (pos >= buf_len) break;
        if ((unsigned char)buf[pos] >> 7) {
            if (pos + 4 > buf_len) break;
            value_len = ((unsigned char)buf[pos] & 0x7F) << 24 |
                        (unsigned char)buf[pos+1] << 16 |
                        (unsigned char)buf[pos+2] << 8 |
                        (unsigned char)buf[pos+3];
            pos += 4;
        } else {
            value_len = (unsigned char)buf[pos];
            pos += 1;
        }

        if (pos + name_len + value_len > buf_len) break;

        char *name = malloc(name_len + 1);
        memcpy(name, buf + pos, name_len);
        name[name_len] = '\0';
        pos += name_len;

        char *value = malloc(value_len + 1);
        memcpy(value, buf + pos, value_len);
        value[value_len] = '\0';
        pos += value_len;

        params[*param_count] = name;
        params[*param_count + 1] = value;
        *param_count += 2;
    }
    return 0;
}

static void fcgi_handle_connection(int client_fd, jscgi_config *cfg) {
    fcgi_header hdr;
    int request_id = 0;

    char *params_buf = NULL;
    size_t params_buf_len = 0;
    size_t params_buf_cap = 0;

    char *stdin_buf = NULL;
    size_t stdin_buf_len = 0;
    size_t stdin_buf_cap = 0;

    int params_done = 0;
    int stdin_done = 0;
    int keep_conn = 0;

    while (fcgi_read_exact(client_fd, &hdr, sizeof(hdr)) == 0) {
        int content_length = (hdr.contentLengthB1 << 8) | hdr.contentLengthB0;
        request_id = (hdr.requestIdB1 << 8) | hdr.requestIdB0;

        char *content = NULL;
        if (content_length > 0) {
            content = malloc(content_length);
            if (fcgi_read_exact(client_fd, content, content_length) < 0) {
                free(content);
                break;
            }
        }

        /* Skip padding */
        if (hdr.paddingLength > 0) {
            char pad[256];
            if (fcgi_read_exact(client_fd, pad, hdr.paddingLength) < 0) {
                free(content);
                break;
            }
        }

        switch (hdr.type) {
            case FCGI_BEGIN_REQUEST:
                params_buf_len = 0;
                stdin_buf_len = 0;
                params_done = 0;
                stdin_done = 0;
                if (content && content_length >= 3) {
                    keep_conn = content[2] & 1;
                }
                break;

            case FCGI_PARAMS:
                if (content_length == 0) {
                    params_done = 1;
                } else {
                    while (params_buf_len + content_length > params_buf_cap) {
                        params_buf_cap = params_buf_cap ? params_buf_cap * 2 : 8192;
                        params_buf = realloc(params_buf, params_buf_cap);
                    }
                    memcpy(params_buf + params_buf_len, content, content_length);
                    params_buf_len += content_length;
                }
                break;

            case FCGI_STDIN:
                if (content_length == 0) {
                    stdin_done = 1;
                } else {
                    while (stdin_buf_len + content_length > stdin_buf_cap) {
                        stdin_buf_cap = stdin_buf_cap ? stdin_buf_cap * 2 : 8192;
                        stdin_buf = realloc(stdin_buf, stdin_buf_cap);
                    }
                    memcpy(stdin_buf + stdin_buf_len, content, content_length);
                    stdin_buf_len += content_length;
                }
                break;

            case FCGI_ABORT_REQUEST:
                free(content);
                goto cleanup;
        }

        free(content);

        /* Once we have all params and stdin, execute */
        if (params_done && stdin_done) {
            /* Decode params */
            char *params[512];
            int param_count = 0;
            fcgi_decode_name_value(params_buf, params_buf_len, params, &param_count, 512);

            /* Build request */
            cgi_request req = {0};
            req.params = params;
            req.param_count = param_count;
            req.body = stdin_buf;
            req.body_len = stdin_buf_len;

            const char *script_path = NULL;
            for (int i = 0; i < param_count; i += 2) {
                if (strcmp(params[i], "REQUEST_METHOD") == 0) req.method = params[i+1];
                else if (strcmp(params[i], "REQUEST_URI") == 0) req.uri = params[i+1];
                else if (strcmp(params[i], "QUERY_STRING") == 0) req.query_string = params[i+1];
                else if (strcmp(params[i], "CONTENT_TYPE") == 0) req.content_type = params[i+1];
                else if (strcmp(params[i], "CONTENT_LENGTH") == 0) req.content_length = params[i+1];
                else if (strcmp(params[i], "HTTP_COOKIE") == 0) req.cookie = params[i+1];
                else if (strcmp(params[i], "SCRIPT_FILENAME") == 0) script_path = params[i+1];
                else if (strcmp(params[i], "PATH_TRANSLATED") == 0 && !script_path) script_path = params[i+1];
            }

            if (!script_path) {
                const char *err = "Status: 500\r\nContent-Type: text/html\r\n\r\n"
                                  "<h1>500</h1><p>No SCRIPT_FILENAME</p>";
                fcgi_write_record(client_fd, FCGI_STDOUT, request_id, err, strlen(err));
                fcgi_write_record(client_fd, FCGI_STDOUT, request_id, NULL, 0);
                fcgi_write_end_request(client_fd, request_id, 1);
            } else {
                /* Set env vars for the engine */
                for (int i = 0; i < param_count; i += 2) {
                    setenv(params[i], params[i+1], 1);
                }

                /* Provide body via stdin pipe */
                int body_pipe[2] = {-1, -1};
                int saved_stdin = -1;
                if (stdin_buf_len > 0) {
                    pipe(body_pipe);
                    (void)write(body_pipe[1], stdin_buf, stdin_buf_len);
                    close(body_pipe[1]);
                    saved_stdin = dup(STDIN_FILENO);
                    dup2(body_pipe[0], STDIN_FILENO);
                    close(body_pipe[0]);
                }

                /* Capture stdout */
                int out_pipe[2];
                pipe(out_pipe);
                int saved_stdout = dup(STDOUT_FILENO);
                dup2(out_pipe[1], STDOUT_FILENO);
                close(out_pipe[1]);

                execute_script(cfg, script_path, &req, 0, NULL);
                fflush(stdout);

                /* Restore stdout */
                dup2(saved_stdout, STDOUT_FILENO);
                close(saved_stdout);

                /* Restore stdin */
                if (saved_stdin >= 0) {
                    dup2(saved_stdin, STDIN_FILENO);
                    close(saved_stdin);
                }

                /* Read captured output */
                char out_buf[1048576];
                size_t out_total = 0;
                ssize_t nr;
                while ((nr = read(out_pipe[0], out_buf + out_total,
                                  sizeof(out_buf) - 1 - out_total)) > 0) {
                    out_total += nr;
                }
                close(out_pipe[0]);

                /* Send via FastCGI */
                if (out_total > 0) {
                    fcgi_write_record(client_fd, FCGI_STDOUT, request_id, out_buf, out_total);
                }
                fcgi_write_record(client_fd, FCGI_STDOUT, request_id, NULL, 0);
                fcgi_write_end_request(client_fd, request_id, 0);

                /* Clear env vars */
                for (int i = 0; i < param_count; i += 2) {
                    unsetenv(params[i]);
                }
            }

            /* Free params */
            for (int i = 0; i < param_count; i++) {
                free(params[i]);
            }

            /* Reset for next request or close connection */
            if (!keep_conn) goto cleanup;
            params_buf_len = 0;
            stdin_buf_len = 0;
            params_done = 0;
            stdin_done = 0;
        }
    }

cleanup:
    free(params_buf);
    free(stdin_buf);
    close(client_fd);
}

static int run_fastcgi(const char *addr, int num_workers, const char *ini_override) {
    jscgi_config cfg;
    config_load(&cfg, ini_override);
    g_cfg = &cfg;

    int server_fd;

    /* Determine if addr is a Unix socket or TCP */
    int is_unix = (addr[0] == '/' || addr[0] == '.');
    struct sockaddr_in tcp_addr;

    if (is_unix) {
        struct sockaddr_un un_addr;
        server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd < 0) {
            fprintf(stderr, "Error: cannot create socket\n");
            return 1;
        }
        memset(&un_addr, 0, sizeof(un_addr));
        un_addr.sun_family = AF_UNIX;
        strncpy(un_addr.sun_path, addr, sizeof(un_addr.sun_path) - 1);
        unlink(addr);

        if (bind(server_fd, (struct sockaddr *)&un_addr, sizeof(un_addr)) < 0) {
            fprintf(stderr, "Error: cannot bind to %s — %s\n", addr, strerror(errno));
            close(server_fd);
            return 1;
        }
        chmod(addr, 0666);
    } else {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            fprintf(stderr, "Error: cannot create socket\n");
            return 1;
        }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        memset(&tcp_addr, 0, sizeof(tcp_addr));
        tcp_addr.sin_family = AF_INET;

        const char *colon = strchr(addr, ':');
        if (colon) {
            char host[256];
            size_t hlen = colon - addr;
            if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
            memcpy(host, addr, hlen);
            host[hlen] = '\0';
            inet_pton(AF_INET, host, &tcp_addr.sin_addr);
            tcp_addr.sin_port = htons(atoi(colon + 1));
        } else {
            tcp_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            tcp_addr.sin_port = htons(atoi(addr));
        }

        if (bind(server_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
            fprintf(stderr, "Error: cannot bind to %s — %s\n", addr, strerror(errno));
            close(server_fd);
            return 1;
        }
    }

    if (listen(server_fd, 128) < 0) {
        fprintf(stderr, "Error: listen failed — %s\n", strerror(errno));
        close(server_fd);
        return 1;
    }

    fprintf(stderr, "js-cgi FastCGI server\n");
    fprintf(stderr, "Listening on %s\n", addr);
    fprintf(stderr, "Workers: %d\n\n", num_workers);

    /* Pre-fork workers */
    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            /* Worker loop */
            while (1) {
                struct sockaddr_storage client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    if (errno == EINTR) continue;
                    _exit(1);
                }
                fcgi_handle_connection(client_fd, &cfg);
            }
        } else if (pid < 0) {
            fprintf(stderr, "Error: fork failed\n");
        }
    }

    /* Master: wait for children, restart if they die */
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGCHLD, &sa, NULL);

    while (1) {
        int status;
        pid_t died = waitpid(-1, &status, 0);
        if (died < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* Restart worker */
        pid_t pid = fork();
        if (pid == 0) {
            while (1) {
                struct sockaddr_storage client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    if (errno == EINTR) continue;
                    _exit(1);
                }
                fcgi_handle_connection(client_fd, &cfg);
            }
        }
    }

    close(server_fd);
    if (is_unix) unlink(addr);

    for (int i = 0; i < cfg.extension_count; i++) {
        free(cfg.extensions[i]);
    }
    return 0;
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

static void handle_request(int client_fd, const char *doc_root, const char *ini_override, const char *router) {
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
        if (router) {
            snprintf(file_path, sizeof(file_path), "%s/%s", doc_root, router);
        } else {
            dprintf(client_fd, "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                    "<h1>404 Not Found</h1><p>%s</p>\n", path);
            fprintf(stderr, "[%s] 404 %s %s\n", method, path, "Not Found");
            return;
        }
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

static int run_dev_server(const char *host, int port, const char *doc_root, const char *ini_override, const char *router) {
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
    if (router) fprintf(stderr, "Router: %s\n", router);
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
            handle_request(client_fd, doc_root, ini_override, router);
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
    const char *fastcgi_arg = NULL;
    const char *router_script = NULL;
    int num_workers = 4;
    int script_argc = 0;
    char **script_argv = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--ini=", 6) == 0) {
            ini_override = argv[i] + 6;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("js-cgi %s\n\n", JSCGI_VERSION);
            printf("Usage: js-cgi [options] <script.js> [script args...]\n\n");
            printf("Options:\n");
            printf("  --serve [host:]port  Start development server (default: 8000)\n");
            printf("  --fastcgi [port]     Start FastCGI server (default: 9000)\n");
            printf("  --workers N          Number of FastCGI workers (default: 4)\n");
            printf("  --router script.js   Route all unmatched requests to this script\n");
            printf("  --ini=path           Load configuration from specified ini file\n");
            printf("  --help               Show this help message\n");
            printf("  --version            Show version number\n");
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", JSCGI_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--serve") == 0) {
            serve_arg = (i + 1 < argc) ? argv[++i] : "8000";
        } else if (strcmp(argv[i], "--fastcgi") == 0) {
            fastcgi_arg = (i + 1 < argc) ? argv[++i] : "9000";
        } else if (strcmp(argv[i], "--workers") == 0) {
            if (i + 1 < argc) num_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--router") == 0) {
            if (i + 1 < argc) router_script = argv[++i];
        } else {
            script_path = argv[i];
            script_argc = argc - i - 1;
            script_argv = (script_argc > 0) ? &argv[i + 1] : NULL;
            break;
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

        return run_dev_server(host, port, doc_root, ini_override, router_script);
    }

    /* FastCGI mode */
    if (fastcgi_arg) {
        if (num_workers < 1) num_workers = 1;
        if (num_workers > 64) num_workers = 64;
        return run_fastcgi(fastcgi_arg, num_workers, ini_override);
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

    int ret = execute_script(&cfg, script_path, NULL, script_argc, script_argv);

    for (int i = 0; i < cfg.extension_count; i++) {
        free(cfg.extensions[i]);
    }

    return ret;
}
