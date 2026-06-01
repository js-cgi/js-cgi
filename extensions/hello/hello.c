#include "../../js-cgi-module.h"

static JSValue js_hello_world(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewString(ctx, "Hello from the hello extension!");
}

static JSValue js_hello_greet(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *name = "World";
    if (argc > 0) {
        name = JS_ToCString(ctx, argv[0]);
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Hello, %s!", name);

    if (argc > 0) {
        JS_FreeCString(ctx, name);
    }

    return JS_NewString(ctx, buf);
}

static int hello_init(JSContext *ctx, JSValue global) {
    JSValue hello = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, hello, "world",
        JS_NewCFunction(ctx, js_hello_world, "world", 0));
    JS_SetPropertyStr(ctx, hello, "greet",
        JS_NewCFunction(ctx, js_hello_greet, "greet", 1));
    JS_SetPropertyStr(ctx, global, "hello", hello);
    return 0;
}

static int hello_shutdown(void) {
    return 0;
}

JSCGI_MODULE(hello, "0.1.0", hello_init, hello_shutdown);
