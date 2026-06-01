#ifndef JSCGI_MODULE_H
#define JSCGI_MODULE_H

#include "quickjs/quickjs.h"

#define JSCGI_MODULE_VERSION "0.1.0"

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
