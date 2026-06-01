# jscgi

A lightweight JavaScript CGI engine. Run modern JavaScript on your web server the same way you run PHP — drop `.js` files in a directory and they're served as web pages.

Powered by QuickJS-ng for full ES2023+ support.

## Features

- Full ES2023+ JavaScript (modules, async/await, generators, destructuring, etc.)
- CGI interface for Apache and Nginx
- `request` and `response` globals for handling HTTP
- `print()` for output
- ES module support (`import`/`export` from local files)
- Shared extension system (`.so` modules like PHP extensions)
- Memory and execution time limits
- Stack traces in error output
- Configurable via ini file

## Requirements

- GCC
- Make
- Git

## Installation

```bash
git clone <repo-url> jscgi
cd jscgi
./build.sh
sudo cp jscgi /usr/lib/cgi-bin/jscgi
sudo mkdir -p /etc/jscgi
sudo cp jscgi.ini /etc/jscgi/jscgi.ini
sudo mkdir -p /usr/lib/jscgi/modules
```

## Apache Configuration

Enable CGI and actions:

```bash
sudo a2enmod cgid actions
```

Create a site config:

```apache
<VirtualHost *:80>
    DocumentRoot /var/www/js

    Action jscgi /cgi-bin/jscgi
    AddHandler jscgi .js

    ScriptAlias /cgi-bin/ /usr/lib/cgi-bin/

    <Directory /var/www/js>
        Options +ExecCGI
        Require all granted
    </Directory>

    <Directory /usr/lib/cgi-bin>
        Require all granted
    </Directory>
</VirtualHost>
```

Enable and restart:

```bash
sudo a2ensite jscgi
sudo systemctl restart apache2
```

## Quick Start

Create `/var/www/js/index.js`:

```js
const name = request.query.name || "World";
print(`<h1>Hello, ${name}!</h1>`);
```

Visit `http://localhost/index.js?name=Developer`

## API

### request

| Property | Description |
|----------|-------------|
| `request.method` | HTTP method (GET, POST, etc.) |
| `request.uri` | Full request URI including query string |
| `request.path` | Path without query string |
| `request.query` | Parsed query string as object |
| `request.headers` | Request headers as object (lowercase keys) |
| `request.body` | Request body as string |

### response

| Method | Description |
|--------|-------------|
| `response.setHeader(name, value)` | Set a response header |
| `response.setStatus(code)` | Set the HTTP status code |

### print()

```js
print("Hello");          // Appends to response body
print("<p>HTML</p>");    // Supports any string
```

Content-Type defaults to `text/html`. Override with:

```js
response.setHeader("Content-Type", "application/json");
```

## ES Modules

Import from local files relative to the script:

```js
import { greet } from "./modules/utils.js";
print(greet("World"));
```

Module files export as normal:

```js
// modules/utils.js
export function greet(name) {
    return `Hello, ${name}!`;
}
```

## Configuration

`/etc/jscgi/jscgi.ini`:

```ini
# Maximum memory a script can use (supports K, M, G suffixes)
memory_limit = 128M

# Maximum execution time in seconds
max_execution_time = 30

# Directory where extensions are loaded from
extension_dir = /usr/lib/jscgi/modules

# Load extensions
extension = example.so
```

### INI load order

1. `/etc/jscgi/jscgi.ini` (system-wide)
2. `jscgi.ini` in the same directory as the binary (local override)
3. `--ini=/path/to/jscgi.ini` flag (takes precedence over both)

## Writing Extensions

Extensions are shared libraries (`.so`) that register JavaScript functions and objects.

### Extension structure

```c
#include "jscgi-module.h"

static JSValue js_my_function(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    return JS_NewString(ctx, "Hello from my extension!");
}

static int my_ext_init(JSContext *ctx, JSValue global) {
    JS_SetPropertyStr(ctx, global, "myFunction",
        JS_NewCFunction(ctx, js_my_function, "myFunction", 0));
    return 0;
}

static int my_ext_shutdown(void) {
    return 0;
}

JSCGI_MODULE(my_ext, "1.0.0", my_ext_init, my_ext_shutdown);
```

### Building an extension

```bash
gcc -shared -fPIC -O2 -I/path/to/jscgi -o my_ext.so my_ext.c
```

### Installing an extension

```bash
sudo cp my_ext.so /usr/lib/jscgi/modules/
```

Add to `/etc/jscgi/jscgi.ini`:

```ini
extension = my_ext.so
```

### Module entry

Every extension must use the `JSCGI_MODULE` macro:

```c
JSCGI_MODULE(name, version, init_function, shutdown_function);
```

| Parameter | Description |
|-----------|-------------|
| `name` | Extension identifier (no quotes) |
| `version` | Version string |
| `init_function` | Called on startup, receives JSContext and global object |
| `shutdown_function` | Called on shutdown (can be NULL) |

The init function receives the QuickJS context and global object. Use the QuickJS C API to register functions, objects, or any JavaScript values.

## License

MIT
