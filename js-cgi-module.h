#ifndef JSCGI_MODULE_H
#define JSCGI_MODULE_H

/*
 * js-cgi Extension API
 *
 * This is the only header needed to write js-cgi extensions.
 * All symbols resolve at runtime from the js-cgi binary.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define JSCGI_MODULE_VERSION "0.1.2"

/*
 * If QuickJS is already included (building js-cgi itself), skip
 * our type definitions — they'd conflict. Only the module API
 * macros at the bottom are needed in that case.
 */
#ifdef QUICKJS_H
/* QuickJS already loaded — skip to module API */
#else

/* -------------------- Types -------------------- */

typedef struct JSRuntime JSRuntime;
typedef struct JSContext JSContext;
typedef uint32_t JSAtom;

/*
 * JSValue representation — platform dependent.
 * On 64-bit systems this is a struct, on 32-bit it's a uint64_t.
 */
#if INTPTR_MAX < INT64_MAX
typedef uint64_t JSValue;
#else
typedef union JSValueUnion {
    int32_t int32;
    double float64;
    void *ptr;
} JSValueUnion;

typedef struct JSValue {
    JSValueUnion u;
    int64_t tag;
} JSValue;
#endif

#define JSValueConst JSValue

typedef struct JSPropertyEnum {
    bool is_enumerable;
    JSAtom atom;
} JSPropertyEnum;

typedef JSValue JSCFunction(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

/* -------------------- Tags -------------------- */

enum {
    JS_TAG_BIG_INT     = -9,
    JS_TAG_SYMBOL      = -8,
    JS_TAG_STRING      = -7,
    JS_TAG_OBJECT      = -1,
    JS_TAG_INT         = 0,
    JS_TAG_BOOL        = 1,
    JS_TAG_NULL        = 2,
    JS_TAG_UNDEFINED   = 3,
    JS_TAG_EXCEPTION   = 6,
    JS_TAG_FLOAT64     = 8,
};

/* -------------------- Value constructors -------------------- */

#if INTPTR_MAX < INT64_MAX
#define JS_MKVAL(tag, val) (((uint64_t)(tag) << 32) | (uint32_t)(val))
#define JS_VALUE_GET_TAG(v) (int)((v) >> 32)
#define JS_VALUE_GET_INT(v) (int)(v)
#define JS_VALUE_GET_FLOAT64(v) __JS_GetFloat64(v)
#define JS_VALUE_GET_NORM_TAG(v) __JS_GetNormTag(v)
#else
#define JS_MKVAL(t, v) ((JSValue){ .u = { .int32 = (v) }, .tag = (int64_t)(t) })
#define JS_VALUE_GET_TAG(v) ((int)(v).tag)
#define JS_VALUE_GET_INT(v) ((v).u.int32)
#define JS_VALUE_GET_FLOAT64(v) ((v).u.float64)
static inline int JS_VALUE_GET_NORM_TAG(JSValue v) {
    int t = (int)v.tag;
    if (t >= JS_TAG_FLOAT64) return JS_TAG_FLOAT64;
    return t;
}
#endif

#define JS_NULL      JS_MKVAL(JS_TAG_NULL, 0)
#define JS_UNDEFINED JS_MKVAL(JS_TAG_UNDEFINED, 0)
#define JS_TRUE      JS_MKVAL(JS_TAG_BOOL, 1)
#define JS_FALSE     JS_MKVAL(JS_TAG_BOOL, 0)
#define JS_EXCEPTION JS_MKVAL(JS_TAG_EXCEPTION, 0)

/* -------------------- Value checks -------------------- */

static inline bool JS_IsNull(JSValueConst v) { return JS_VALUE_GET_TAG(v) == JS_TAG_NULL; }
static inline bool JS_IsUndefined(JSValueConst v) { return JS_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED; }
static inline bool JS_IsException(JSValueConst v) { return JS_VALUE_GET_TAG(v) == JS_TAG_EXCEPTION; }
static inline bool JS_IsString(JSValueConst v) { return JS_VALUE_GET_TAG(v) == JS_TAG_STRING; }
static inline bool JS_IsObject(JSValueConst v) { return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT; }

/* -------------------- Constants -------------------- */

#define JS_GPN_STRING_MASK  (1 << 0)
#define JS_GPN_ENUM_ONLY    (1 << 4)

/* -------------------- Function declarations -------------------- */

/* Memory */
extern void JS_FreeValue(JSContext *ctx, JSValue v);
extern JSValue JS_DupValue(JSContext *ctx, JSValueConst v);
extern void JS_FreeAtom(JSContext *ctx, JSAtom v);
extern void JS_FreeCString(JSContext *ctx, const char *ptr);
extern void js_free(JSContext *ctx, void *ptr);

/* Value creation */
extern JSValue JS_NewObject(JSContext *ctx);
extern JSValue JS_NewArray(JSContext *ctx);
extern JSValue JS_NewStringLen(JSContext *ctx, const char *str, size_t len);
extern JSValue JS_NewCFunction2(JSContext *ctx, JSCFunction *func,
                                const char *name, int length, int cproto, int magic);

static inline JSValue JS_NewCFunction(JSContext *ctx, JSCFunction *func,
                                      const char *name, int length) {
    return JS_NewCFunction2(ctx, func, name, length, 0 /* JS_CFUNC_generic */, 0);
}

static inline JSValue JS_NewString(JSContext *ctx, const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return JS_NewStringLen(ctx, str, len);
}

static inline JSValue JS_NewBool(JSContext *ctx, bool val) {
    return JS_MKVAL(JS_TAG_BOOL, val != 0);
}

static inline JSValue JS_NewInt32(JSContext *ctx, int32_t val) {
    return JS_MKVAL(JS_TAG_INT, val);
}

static inline JSValue JS_NewFloat64(JSContext *ctx, double val) {
    (void)ctx;
#if INTPTR_MAX < INT64_MAX
    union { double d; uint64_t u64; } u;
    u.d = val;
    return u.u64;
#else
    JSValue v;
    v.tag = JS_TAG_FLOAT64;
    v.u.float64 = val;
    return v;
#endif
}

static inline JSValue JS_NewInt64(JSContext *ctx, int64_t val) {
    if (val >= INT32_MIN && val <= INT32_MAX) {
        return JS_NewInt32(ctx, (int32_t)val);
    } else {
        return JS_NewFloat64(ctx, (double)val);
    }
}

/* Value conversion */
extern const char *JS_ToCStringLen2(JSContext *ctx, size_t *plen, JSValueConst val, bool cesu8);
extern int JS_ToInt32(JSContext *ctx, int32_t *pres, JSValueConst val);
extern int JS_ToInt64(JSContext *ctx, int64_t *pres, JSValueConst val);
extern int JS_ToFloat64(JSContext *ctx, double *pres, JSValueConst val);

static inline const char *JS_ToCString(JSContext *ctx, JSValueConst val) {
    return JS_ToCStringLen2(ctx, NULL, val, 0);
}

static inline const char *JS_ToCStringLen(JSContext *ctx, size_t *plen, JSValueConst val) {
    return JS_ToCStringLen2(ctx, plen, val, 0);
}

/* Properties */
extern int JS_SetPropertyStr(JSContext *ctx, JSValueConst this_obj, const char *prop, JSValue val);
extern JSValue JS_GetPropertyStr(JSContext *ctx, JSValueConst this_obj, const char *prop);
extern int JS_SetPropertyUint32(JSContext *ctx, JSValueConst this_obj, uint32_t idx, JSValue val);
extern JSValue JS_GetPropertyUint32(JSContext *ctx, JSValueConst this_obj, uint32_t idx);
extern JSValue JS_GetProperty(JSContext *ctx, JSValueConst this_obj, JSAtom prop);
extern int JS_GetOwnPropertyNames(JSContext *ctx, JSPropertyEnum **ptab, uint32_t *plen, JSValueConst obj, int flags);
extern const char *JS_AtomToCStringLen(JSContext *ctx, size_t *plen, JSAtom atom);

static inline const char *JS_AtomToCString(JSContext *ctx, JSAtom atom) {
    return JS_AtomToCStringLen(ctx, NULL, atom);
}

/* Arrays */
extern bool JS_IsArray(JSValueConst val);

/* JSON */
extern JSValue JS_ParseJSON(JSContext *ctx, const char *buf, size_t buf_len, const char *filename);
extern JSValue JS_JSONStringify(JSContext *ctx, JSValueConst obj, JSValueConst replacer, JSValueConst space);

/* Errors */
extern JSValue JS_ThrowTypeError(JSContext *ctx, const char *fmt, ...);
extern JSValue JS_ThrowReferenceError(JSContext *ctx, const char *fmt, ...);

#endif /* !QUICKJS_H */

/* -------------------- js-cgi Module API -------------------- */

typedef struct {
    const char *name;
    const char *version;
    int (*module_init)(JSContext *ctx, JSValue global);
    int (*module_shutdown)(void);
} jscgi_module_entry;

#define JSCGI_MODULE(mod_name, mod_version, init_func, shutdown_func) \
    jscgi_module_entry jscgi_module_entry_##mod_name = { \
        #mod_name, \
        mod_version, \
        init_func, \
        shutdown_func \
    }; \
    jscgi_module_entry *jscgi_get_module(void) { \
        return &jscgi_module_entry_##mod_name; \
    }

#endif
