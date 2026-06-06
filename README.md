# js-cgi

A lightweight JavaScript CGI engine. Run modern JavaScript on your web server — drop `.js` files in a directory and they're served as web pages.

Full ES2023+ JavaScript support. No Node.js required.

**Website:** [js-cgi.com](https://js-cgi.com) — documentation, downloads, and examples. The site itself is built with and powered by js-cgi v0.1.0.

## About

js-cgi was created by Andrew Stubbs, a developer who has been writing software since the age of 10. The idea first came about around 15 years ago while learning PHP — the thought was simple: why can't I just write web pages in JavaScript the same way I write them in PHP?

At the time, server-side JavaScript wasn't really a thing. Then Node.js came along and changed everything — but it brought with it a world of frameworks, package managers, and build tools. The original idea of just dropping a script on a server and having it run was lost somewhere along the way.

js-cgi brings that simplicity back. No runtime to manage, no dependencies to install, no build step. Just JavaScript, a web server, and your code.

## Roadmap

See [ROADMAP.md](ROADMAP.md) for planned features including FastCGI support, a built-in development server, and macOS/Windows support.

## Features

- Full ES2023+ JavaScript (modules, async/await, generators, destructuring, etc.)
- CGI interface for Apache and Nginx
- `request` and `response` globals for handling HTTP
- `print()` and `include()` built-ins
- ES module support (`import`/`export` from local files)
- Cookie and session support
- Shared extension system (`.so` modules like PHP extensions)
- Memory and execution time limits
- Configurable error display and logging
- Configurable via ini file

## Platform Support

- **Linux** (x86_64, arm64) — fully supported, pre-built binaries available
- **macOS** — coming soon
- **Windows** — coming soon

## Requirements (Linux)

- GCC
- Make
- Git
- libsqlite3-dev (for sqlite extension)
- libssl-dev (for crypto extension)
- libcurl4-openssl-dev (for http extension)

## Installation

```bash
git clone <repo-url> js-cgi
cd js-cgi
./build.sh
sudo cp js-cgi /usr/lib/cgi-bin/js-cgi
sudo mkdir -p /etc/js-cgi
sudo cp js-cgi.ini /etc/js-cgi/js-cgi.ini
sudo mkdir -p /usr/lib/js-cgi/modules
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

    Action js-cgi /cgi-bin/js-cgi
    AddHandler js-cgi .js

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
sudo a2ensite js-cgi
sudo systemctl restart apache2
```

## Quick Start

Create a file called `index.js`:

```js
const name = request.query.name || "World";
print(`<h1>Hello, ${name}!</h1>`);
```

Start the development server:

```bash
js-cgi --serve 8000
```

Visit `http://localhost:8000/?name=Developer`

## Development Server

js-cgi includes a built-in development server. No Apache or Nginx required for local development.

```bash
# Start on default port (8000)
js-cgi --serve

# Specify port
js-cgi --serve 3000

# Specify host and port
js-cgi --serve 0.0.0.0:8080

# Specify document root
js-cgi --serve 8000 /path/to/project

# With a custom ini file
js-cgi --ini=/path/to/js-cgi.ini --serve 8000

# With a router script (for MVC/front-controller apps)
js-cgi --serve 8000 --router index.js
```

The dev server:
- Runs `.js` files through the js-cgi engine
- Serves static files (HTML, CSS, images, etc.) directly
- Uses `index.js` as the directory index (falls back to `index.html`)
- Forks per request (same isolation as production CGI)
- Logs requests to the terminal

### Router Script

By default, the dev server returns 404 when no file matches the requested path. The `--router` flag specifies a fallback script that handles unmatched requests:

```bash
js-cgi --serve 8000 --router index.js
```

When `--router` is set:
1. If the request matches a static file, it is served directly
2. If the request matches a `.js` file, that file is executed
3. Otherwise, the router script is executed with the original request URI available via `request.path`

This is useful for applications that define their own URL routes in code rather than mapping URLs to individual files on disk. The router script receives the full request context (`request.method`, `request.path`, `request.query`, etc.) and is responsible for dispatching to the appropriate handler.

## FastCGI

For production deployments, js-cgi supports FastCGI — a persistent process mode that avoids the overhead of spawning a new process per request.

```bash
# Listen on a TCP port
js-cgi --fastcgi 9000

# Listen on a Unix socket
js-cgi --fastcgi /var/run/js-cgi.sock

# Specify host and port
js-cgi --fastcgi 127.0.0.1:9000

# Set number of worker processes (default: 4)
js-cgi --fastcgi 9000 --workers 8

# With a custom ini file
js-cgi --ini=/etc/js-cgi/js-cgi.ini --fastcgi /var/run/js-cgi.sock --workers 4
```

### Nginx configuration

```nginx
server {
    listen 80;
    root /var/www/js;

    location ~ \.js$ {
        fastcgi_pass unix:/var/run/js-cgi.sock;
        # Or: fastcgi_pass 127.0.0.1:9000;
        include fastcgi_params;
        fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;
    }
}
```

### Apache configuration (mod_proxy_fcgi)

```apache
<VirtualHost *:80>
    DocumentRoot /var/www/js

    <FilesMatch "\.js$">
        SetHandler "proxy:fcgi://127.0.0.1:9000"
    </FilesMatch>
</VirtualHost>
```

## Core API

### request

| Property | Description |
|----------|-------------|
| `request.method` | HTTP method (GET, POST, etc.) |
| `request.uri` | Full request URI including query string |
| `request.path` | Path without query string |
| `request.query` | Parsed query string as object |
| `request.headers` | Request headers as object (lowercase keys) |
| `request.body` | Request body as string |
| `request.cookies` | Parsed cookies as object |

### response

| Method | Description |
|--------|-------------|
| `response.setHeader(name, value)` | Set a response header |
| `response.setStatus(code)` | Set the HTTP status code |
| `response.setCookie(name, value, options?)` | Set a cookie |

Cookie options: `{ path, maxAge, httpOnly, secure, sameSite }`

### print()

```js
print("Hello");          // Appends to response body
print("<p>HTML</p>");    // Supports any string
```

Content-Type defaults to `text/html`. Override with:

```js
response.setHeader("Content-Type", "application/json");
```

### include()

```js
include("./header.js");
print("<p>Page content</p>");
include("./footer.js");
```

Resolves paths relative to the current script. Included files share the same context.

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

## Extensions

### Session (session.so)

File-backed sessions with cookie-based session IDs.

```js
session.start();
session.set("user", "Alice");
session.get("user");    // "Alice"
session.destroy();      // End session
```

### SQLite (sqlite.so)

Embedded SQLite database access with parameterised queries.

```js
const db = sqlite.open("/var/www/data/app.db");

db.exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT)");
db.exec("INSERT INTO users (name) VALUES (?)", ["Alice"]);

const users = db.query("SELECT * FROM users");
const user = db.queryOne("SELECT * FROM users WHERE id = ?", [1]);

db.lastInsertId();      // Last auto-increment ID
db.changes();           // Rows affected by last statement
db.tableExists("users"); // true/false

db.beginTransaction();
db.exec("INSERT INTO users (name) VALUES (?)", ["Bob"]);
db.commit();            // or db.rollback()

db.close();
```

### File (file.so)

Filesystem access.

```js
file.write("/tmp/data.txt", "Hello");
file.append("/tmp/data.txt", "\nWorld");
file.read("/tmp/data.txt");     // "Hello\nWorld"
file.exists("/tmp/data.txt");   // true
file.size("/tmp/data.txt");     // 11
file.isDir("/tmp");             // true
file.list("/tmp");              // ["data.txt", ...]
file.delete("/tmp/data.txt");
```

### Crypto (crypto.so)

Hashing, HMAC, and random generation.

```js
crypto.md5("hello");        // hex string
crypto.sha1("hello");       // hex string
crypto.sha256("hello");     // hex string
crypto.sha512("hello");     // hex string

crypto.hmac("sha256", "secret-key", "data");  // hex string

crypto.randomBytes(16);     // 32-char hex string (16 bytes)
crypto.uuid();              // v4 UUID
```

### HTTP (http.so)

Make outbound HTTP requests.

```js
const res = http.get("https://api.example.com/data");
// res.status, res.body, res.headers

const res = http.post(
    "https://api.example.com/data",
    JSON.stringify({ name: "Alice" }),
    { "Content-Type": "application/json" }
);

http.put(url, body, headers);
http.delete(url, headers);
```

All methods return `{ status, body, headers }`.

## Configuration

`/etc/js-cgi/js-cgi.ini`:

```ini
# Maximum memory a script can use (supports K, M, G suffixes)
memory_limit = 128M

# Maximum execution time in seconds
max_execution_time = 30

# Show errors in browser (On) or hide them (Off)
display_errors = On

# Log errors to file
error_log = /var/log/js-cgi/error.log

# Directory where extensions are loaded from
extension_dir = /usr/lib/js-cgi/modules

# Load extensions
extension = session.so
extension = sqlite.so
extension = file.so
extension = crypto.so
extension = http.so
```

### INI load order

1. `/etc/js-cgi/js-cgi.ini` (system-wide)
2. `js-cgi.ini` in the same directory as the binary (local override)
3. `--ini=/path/to/js-cgi.ini` flag (takes precedence over both)

## Writing Extensions

Extensions are shared libraries (`.so`) that register JavaScript functions and objects.

### Extension structure

```c
#include "js-cgi-module.h"

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

The only file needed is `js-cgi-module.h` (installed to `/usr/include/js-cgi/` or included in the download archive):

```bash
gcc -shared -fPIC -O2 -I/usr/include/js-cgi -o my_ext.so my_ext.c
```

### Installing an extension

```bash
sudo cp my_ext.so /usr/lib/js-cgi/modules/
```

Add to `/etc/js-cgi/js-cgi.ini`:

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

The init function receives the js-cgi context and global object. Use the js-cgi C API (defined in `js-cgi-module.h`) to register functions, objects, or any JavaScript values.

## License

MIT
