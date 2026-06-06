#include "../../js-cgi-module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

extern char g_script_dir[4096];

static const char *resolve_path(const char *path, char *buf, size_t buf_size) {
    if (path[0] == '/') return path;
    snprintf(buf, buf_size, "%s/%s", g_script_dir, path);
    return buf;
}

/* -------------------- JS API: file.read() -------------------- */

static JSValue js_file_read(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "file.read requires a file path");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char resolved[8192];
    const char *full_path = resolve_path(path, resolved, sizeof(resolved));

    FILE *fp = fopen(full_path, "rb");
    if (!fp) {
        JSValue err = JS_ThrowTypeError(ctx, "Cannot read file '%s'", path);
        JS_FreeCString(ctx, path);
        return err;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    size_t read = fread(buf, 1, len, fp);
    buf[read] = '\0';
    fclose(fp);
    JS_FreeCString(ctx, path);

    JSValue result = JS_NewStringLen(ctx, buf, read);
    free(buf);
    return result;
}

/* -------------------- JS API: file.write() -------------------- */

static JSValue js_file_write(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "file.write requires a file path and content");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char resolved[8192];
    const char *full_path = resolve_path(path, resolved, sizeof(resolved));

    size_t content_len;
    const char *content = JS_ToCStringLen(ctx, &content_len, argv[1]);
    if (!content) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }

    FILE *fp = fopen(full_path, "wb");
    if (!fp) {
        JSValue err = JS_ThrowTypeError(ctx, "Cannot write to file '%s'", path);
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, content);
        return err;
    }

    fwrite(content, 1, content_len, fp);
    fclose(fp);

    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, content);
    return JS_TRUE;
}

/* -------------------- JS API: file.append() -------------------- */

static JSValue js_file_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "file.append requires a file path and content");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char resolved[8192];
    const char *full_path = resolve_path(path, resolved, sizeof(resolved));

    size_t content_len;
    const char *content = JS_ToCStringLen(ctx, &content_len, argv[1]);
    if (!content) {
        JS_FreeCString(ctx, path);
        return JS_EXCEPTION;
    }

    FILE *fp = fopen(full_path, "ab");
    if (!fp) {
        JSValue err = JS_ThrowTypeError(ctx, "Cannot append to file '%s'", path);
        JS_FreeCString(ctx, path);
        JS_FreeCString(ctx, content);
        return err;
    }

    fwrite(content, 1, content_len, fp);
    fclose(fp);

    JS_FreeCString(ctx, path);
    JS_FreeCString(ctx, content);
    return JS_TRUE;
}

/* -------------------- JS API: file.exists() -------------------- */

static JSValue js_file_exists(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "file.exists requires a file path");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char resolved[8192];
    const char *full_path = resolve_path(path, resolved, sizeof(resolved));

    struct stat st;
    int exists = (stat(full_path, &st) == 0);
    JS_FreeCString(ctx, path);

    return JS_NewBool(ctx, exists);
}

/* -------------------- JS API: file.delete() -------------------- */

static JSValue js_file_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "file.delete requires a file path");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char resolved[8192];
    const char *full_path = resolve_path(path, resolved, sizeof(resolved));

    int result = unlink(full_path);
    JS_FreeCString(ctx, path);

    if (result != 0) {
        return JS_ThrowTypeError(ctx, "Cannot delete file");
    }

    return JS_TRUE;
}

/* -------------------- JS API: file.size() -------------------- */

static JSValue js_file_size(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "file.size requires a file path");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char resolved[8192];
    const char *full_path = resolve_path(path, resolved, sizeof(resolved));

    struct stat st;
    if (stat(full_path, &st) != 0) {
        JSValue err = JS_ThrowTypeError(ctx, "Cannot stat file '%s'", path);
        JS_FreeCString(ctx, path);
        return err;
    }

    JS_FreeCString(ctx, path);
    return JS_NewInt64(ctx, st.st_size);
}

/* -------------------- JS API: file.isDir() -------------------- */

static JSValue js_file_is_dir(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "file.isDir requires a path");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char resolved[8192];
    const char *full_path = resolve_path(path, resolved, sizeof(resolved));

    struct stat st;
    int is_dir = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
    JS_FreeCString(ctx, path);

    return JS_NewBool(ctx, is_dir);
}

/* -------------------- JS API: file.list() -------------------- */

static JSValue js_file_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "file.list requires a directory path");
    }

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    char resolved[8192];
    const char *full_path = resolve_path(path, resolved, sizeof(resolved));

    DIR *dir = opendir(full_path);
    if (!dir) {
        JSValue err = JS_ThrowTypeError(ctx, "Cannot open directory '%s'", path);
        JS_FreeCString(ctx, path);
        return err;
    }

    JS_FreeCString(ctx, path);

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, entry->d_name));
    }

    closedir(dir);
    return arr;
}

/* -------------------- Module entry -------------------- */

static int file_init(JSContext *ctx, JSValue global) {
    JSValue file_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, file_obj, "read",
        JS_NewCFunction(ctx, js_file_read, "read", 1));
    JS_SetPropertyStr(ctx, file_obj, "write",
        JS_NewCFunction(ctx, js_file_write, "write", 2));
    JS_SetPropertyStr(ctx, file_obj, "append",
        JS_NewCFunction(ctx, js_file_append, "append", 2));
    JS_SetPropertyStr(ctx, file_obj, "exists",
        JS_NewCFunction(ctx, js_file_exists, "exists", 1));
    JS_SetPropertyStr(ctx, file_obj, "delete",
        JS_NewCFunction(ctx, js_file_delete, "delete", 1));
    JS_SetPropertyStr(ctx, file_obj, "size",
        JS_NewCFunction(ctx, js_file_size, "size", 1));
    JS_SetPropertyStr(ctx, file_obj, "isDir",
        JS_NewCFunction(ctx, js_file_is_dir, "isDir", 1));
    JS_SetPropertyStr(ctx, file_obj, "list",
        JS_NewCFunction(ctx, js_file_list, "list", 1));
    JS_SetPropertyStr(ctx, global, "file", file_obj);

    return 0;
}

static int file_shutdown(void) {
    return 0;
}

JSCGI_MODULE(file, "0.1.0", file_init, file_shutdown);
