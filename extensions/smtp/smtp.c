#include "../../js-cgi-module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

#define SMTP_BUF_SIZE 4096
#define MAX_SMTP_CONNS 8

typedef struct {
    int fd;
    SSL_CTX *ssl_ctx;
    SSL *ssl;
    int use_ssl;
} smtp_conn;

static smtp_conn *g_conns[MAX_SMTP_CONNS];
static int g_conn_count = 0;

/* -------------------- Helpers -------------------- */

static int find_conn_slot(void) {
    for (int i = 0; i < MAX_SMTP_CONNS; i++) {
        if (!g_conns[i]) return i;
    }
    return -1;
}

static char *base64_encode(const char *input, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3) + 1;
    char *output = malloc(out_len);
    if (!output) return NULL;

    EVP_ENCODE_CTX *ectx = EVP_ENCODE_CTX_new();
    EVP_EncodeInit(ectx);

    int tmp_len = 0;
    int total = 0;
    EVP_EncodeUpdate(ectx, (unsigned char *)output, &tmp_len,
                     (const unsigned char *)input, len);
    total += tmp_len;
    EVP_EncodeFinal(ectx, (unsigned char *)output + total, &tmp_len);
    total += tmp_len;
    EVP_ENCODE_CTX_free(ectx);

    /* Remove newlines that EVP_Encode adds */
    char *clean = malloc(total + 1);
    int j = 0;
    for (int i = 0; i < total; i++) {
        if (output[i] != '\n' && output[i] != '\r') {
            clean[j++] = output[i];
        }
    }
    clean[j] = '\0';
    free(output);
    return clean;
}

static int smtp_read_response(smtp_conn *conn, char *buf, size_t buf_size) {
    int n;
    if (conn->use_ssl && conn->ssl) {
        n = SSL_read(conn->ssl, buf, buf_size - 1);
    } else {
        n = read(conn->fd, buf, buf_size - 1);
    }
    if (n <= 0) return -1;
    buf[n] = '\0';
    return atoi(buf);
}

static int smtp_send_cmd(smtp_conn *conn, const char *cmd) {
    size_t len = strlen(cmd);
    if (conn->use_ssl && conn->ssl) {
        SSL_write(conn->ssl, cmd, len);
    } else {
        write(conn->fd, cmd, len);
    }
    return 0;
}

static int smtp_send_cmd_expect(smtp_conn *conn, const char *cmd, int expected) {
    smtp_send_cmd(conn, cmd);
    char buf[SMTP_BUF_SIZE];
    int code = smtp_read_response(conn, buf, sizeof(buf));
    return (code == expected) ? 0 : -1;
}

static int tcp_connect(const char *host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

static int smtp_starttls(smtp_conn *conn) {
    if (smtp_send_cmd_expect(conn, "STARTTLS\r\n", 220) != 0) return -1;

    conn->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!conn->ssl_ctx) return -1;

    conn->ssl = SSL_new(conn->ssl_ctx);
    SSL_set_fd(conn->ssl, conn->fd);

    if (SSL_connect(conn->ssl) <= 0) return -1;

    conn->use_ssl = 1;
    return 0;
}

static int smtp_auth_login(smtp_conn *conn, const char *user, const char *pass) {
    char buf[SMTP_BUF_SIZE];

    if (smtp_send_cmd_expect(conn, "AUTH LOGIN\r\n", 334) != 0) return -1;

    char *b64_user = base64_encode(user, strlen(user));
    char cmd[SMTP_BUF_SIZE];
    snprintf(cmd, sizeof(cmd), "%s\r\n", b64_user);
    free(b64_user);
    if (smtp_send_cmd_expect(conn, cmd, 334) != 0) return -1;

    char *b64_pass = base64_encode(pass, strlen(pass));
    snprintf(cmd, sizeof(cmd), "%s\r\n", b64_pass);
    free(b64_pass);

    smtp_send_cmd(conn, cmd);
    int code = smtp_read_response(conn, buf, sizeof(buf));
    if (code != 235) return -1;

    return 0;
}

static smtp_conn *smtp_do_connect(const char *host, int port, const char *user, const char *pass) {
    smtp_conn *conn = calloc(1, sizeof(smtp_conn));
    if (!conn) return NULL;

    conn->fd = tcp_connect(host, port);
    if (conn->fd < 0) {
        free(conn);
        return NULL;
    }

    char buf[SMTP_BUF_SIZE];
    int code;

    /* Direct TLS on port 465 — handshake before greeting */
    if (port == 465) {
        conn->ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!conn->ssl_ctx) {
            close(conn->fd);
            free(conn);
            return NULL;
        }
        conn->ssl = SSL_new(conn->ssl_ctx);
        SSL_set_fd(conn->ssl, conn->fd);
        if (SSL_connect(conn->ssl) <= 0) {
            SSL_free(conn->ssl);
            SSL_CTX_free(conn->ssl_ctx);
            close(conn->fd);
            free(conn);
            return NULL;
        }
        conn->use_ssl = 1;
    }

    /* Read greeting */
    code = smtp_read_response(conn, buf, sizeof(buf));
    if (code != 220) {
        if (conn->ssl) SSL_free(conn->ssl);
        if (conn->ssl_ctx) SSL_CTX_free(conn->ssl_ctx);
        close(conn->fd);
        free(conn);
        return NULL;
    }

    /* EHLO */
    char ehlo[256];
    snprintf(ehlo, sizeof(ehlo), "EHLO localhost\r\n");
    smtp_send_cmd(conn, ehlo);
    code = smtp_read_response(conn, buf, sizeof(buf));
    if (code != 250) {
        close(conn->fd);
        free(conn);
        return NULL;
    }

    /* STARTTLS for port 587 */
    if (port != 465 && !conn->use_ssl) {
        if (smtp_starttls(conn) != 0) {
            close(conn->fd);
            free(conn);
            return NULL;
        }
        /* Re-EHLO after STARTTLS */
        smtp_send_cmd(conn, ehlo);
        code = smtp_read_response(conn, buf, sizeof(buf));
        if (code != 250) {
            SSL_free(conn->ssl);
            SSL_CTX_free(conn->ssl_ctx);
            close(conn->fd);
            free(conn);
            return NULL;
        }
    }

    /* Authenticate */
    if (user && pass && strlen(user) > 0) {
        if (smtp_auth_login(conn, user, pass) != 0) {
            if (conn->ssl) SSL_free(conn->ssl);
            if (conn->ssl_ctx) SSL_CTX_free(conn->ssl_ctx);
            close(conn->fd);
            free(conn);
            return NULL;
        }
    }

    return conn;
}

static int smtp_do_send(smtp_conn *conn, const char *from, const char *to, const char *subject, const char *body) {
    char cmd[SMTP_BUF_SIZE];
    char buf[SMTP_BUF_SIZE];

    snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>\r\n", from);
    if (smtp_send_cmd_expect(conn, cmd, 250) != 0) return -1;

    snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", to);
    if (smtp_send_cmd_expect(conn, cmd, 250) != 0) return -1;

    if (smtp_send_cmd_expect(conn, "DATA\r\n", 354) != 0) return -1;

    /* Send headers and body */
    snprintf(cmd, sizeof(cmd), "From: %s\r\nTo: %s\r\nSubject: %s\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n\r\n", from, to, subject);
    smtp_send_cmd(conn, cmd);
    smtp_send_cmd(conn, body);
    smtp_send_cmd(conn, "\r\n.\r\n");

    int code = smtp_read_response(conn, buf, sizeof(buf));
    return (code == 250) ? 0 : -1;
}

static void smtp_do_close(smtp_conn *conn) {
    if (!conn) return;
    smtp_send_cmd(conn, "QUIT\r\n");
    if (conn->ssl) {
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    if (conn->ssl_ctx) SSL_CTX_free(conn->ssl_ctx);
    if (conn->fd >= 0) close(conn->fd);
    free(conn);
}

/* -------------------- JS API: conn.send() -------------------- */

static JSValue js_smtp_conn_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "send requires an options object");
    }

    JSValue id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int id = JS_VALUE_GET_INT(id_val);
    JS_FreeValue(ctx, id_val);

    if (id < 0 || id >= MAX_SMTP_CONNS || !g_conns[id]) {
        return JS_ThrowTypeError(ctx, "SMTP connection is closed");
    }

    JSValue opts = argv[0];
    JSValue v_from = JS_GetPropertyStr(ctx, opts, "from");
    JSValue v_to = JS_GetPropertyStr(ctx, opts, "to");
    JSValue v_subject = JS_GetPropertyStr(ctx, opts, "subject");
    JSValue v_body = JS_GetPropertyStr(ctx, opts, "body");

    const char *from = JS_ToCString(ctx, v_from);
    const char *to = JS_ToCString(ctx, v_to);
    const char *subject = JS_ToCString(ctx, v_subject);
    const char *body = JS_ToCString(ctx, v_body);

    JS_FreeValue(ctx, v_from);
    JS_FreeValue(ctx, v_to);
    JS_FreeValue(ctx, v_subject);
    JS_FreeValue(ctx, v_body);

    if (!from || !to || !subject || !body) {
        if (from) JS_FreeCString(ctx, from);
        if (to) JS_FreeCString(ctx, to);
        if (subject) JS_FreeCString(ctx, subject);
        if (body) JS_FreeCString(ctx, body);
        return JS_ThrowTypeError(ctx, "send requires from, to, subject, and body");
    }

    int result = smtp_do_send(g_conns[id], from, to, subject, body);

    JS_FreeCString(ctx, from);
    JS_FreeCString(ctx, to);
    JS_FreeCString(ctx, subject);
    JS_FreeCString(ctx, body);

    if (result != 0) {
        return JS_ThrowTypeError(ctx, "SMTP send failed");
    }

    return JS_TRUE;
}

/* -------------------- JS API: conn.close() -------------------- */

static JSValue js_smtp_conn_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue id_val = JS_GetPropertyStr(ctx, this_val, "_id");
    int id = JS_VALUE_GET_INT(id_val);
    JS_FreeValue(ctx, id_val);

    if (id >= 0 && id < MAX_SMTP_CONNS && g_conns[id]) {
        smtp_do_close(g_conns[id]);
        g_conns[id] = NULL;
    }

    return JS_UNDEFINED;
}

/* -------------------- JS API: smtp.connect() -------------------- */

static JSValue js_smtp_connect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 4) {
        return JS_ThrowTypeError(ctx, "smtp.connect requires host, port, user, password");
    }

    int slot = find_conn_slot();
    if (slot < 0) {
        return JS_ThrowTypeError(ctx, "Maximum number of SMTP connections reached");
    }

    const char *host = JS_ToCString(ctx, argv[0]);
    int32_t port;
    JS_ToInt32(ctx, &port, argv[1]);
    const char *user = JS_ToCString(ctx, argv[2]);
    const char *pass = JS_ToCString(ctx, argv[3]);

    if (!host || !user || !pass) {
        if (host) JS_FreeCString(ctx, host);
        if (user) JS_FreeCString(ctx, user);
        if (pass) JS_FreeCString(ctx, pass);
        return JS_EXCEPTION;
    }

    smtp_conn *conn = smtp_do_connect(host, port, user, pass);

    JS_FreeCString(ctx, host);
    JS_FreeCString(ctx, user);
    JS_FreeCString(ctx, pass);

    if (!conn) {
        return JS_ThrowTypeError(ctx, "SMTP connection failed");
    }

    g_conns[slot] = conn;
    if (slot >= g_conn_count) g_conn_count = slot + 1;

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "_id", JS_NewInt32(ctx, slot));
    JS_SetPropertyStr(ctx, obj, "send",
        JS_NewCFunction(ctx, js_smtp_conn_send, "send", 1));
    JS_SetPropertyStr(ctx, obj, "close",
        JS_NewCFunction(ctx, js_smtp_conn_close, "close", 0));

    return obj;
}

/* -------------------- JS API: smtp.send() (one-shot) -------------------- */

static JSValue js_smtp_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "smtp.send requires an options object");
    }

    JSValue opts = argv[0];
    JSValue v_host = JS_GetPropertyStr(ctx, opts, "host");
    JSValue v_port = JS_GetPropertyStr(ctx, opts, "port");
    JSValue v_user = JS_GetPropertyStr(ctx, opts, "user");
    JSValue v_pass = JS_GetPropertyStr(ctx, opts, "password");
    JSValue v_from = JS_GetPropertyStr(ctx, opts, "from");
    JSValue v_to = JS_GetPropertyStr(ctx, opts, "to");
    JSValue v_subject = JS_GetPropertyStr(ctx, opts, "subject");
    JSValue v_body = JS_GetPropertyStr(ctx, opts, "body");

    const char *host = JS_ToCString(ctx, v_host);
    int32_t port = 587;
    if (!JS_IsUndefined(v_port)) JS_ToInt32(ctx, &port, v_port);
    const char *user = JS_ToCString(ctx, v_user);
    const char *pass = JS_ToCString(ctx, v_pass);
    const char *from = JS_ToCString(ctx, v_from);
    const char *to = JS_ToCString(ctx, v_to);
    const char *subject = JS_ToCString(ctx, v_subject);
    const char *body = JS_ToCString(ctx, v_body);

    JS_FreeValue(ctx, v_host);
    JS_FreeValue(ctx, v_port);
    JS_FreeValue(ctx, v_user);
    JS_FreeValue(ctx, v_pass);
    JS_FreeValue(ctx, v_from);
    JS_FreeValue(ctx, v_to);
    JS_FreeValue(ctx, v_subject);
    JS_FreeValue(ctx, v_body);

    if (!host || !from || !to || !subject || !body) {
        if (host) JS_FreeCString(ctx, host);
        if (user) JS_FreeCString(ctx, user);
        if (pass) JS_FreeCString(ctx, pass);
        if (from) JS_FreeCString(ctx, from);
        if (to) JS_FreeCString(ctx, to);
        if (subject) JS_FreeCString(ctx, subject);
        if (body) JS_FreeCString(ctx, body);
        return JS_ThrowTypeError(ctx, "smtp.send requires host, from, to, subject, and body");
    }

    smtp_conn *conn = smtp_do_connect(host, port, user, pass);

    JS_FreeCString(ctx, host);

    if (!conn) {
        if (user) JS_FreeCString(ctx, user);
        if (pass) JS_FreeCString(ctx, pass);
        JS_FreeCString(ctx, from);
        JS_FreeCString(ctx, to);
        JS_FreeCString(ctx, subject);
        JS_FreeCString(ctx, body);
        return JS_ThrowTypeError(ctx, "SMTP connection failed");
    }

    if (user) JS_FreeCString(ctx, user);
    if (pass) JS_FreeCString(ctx, pass);

    int result = smtp_do_send(conn, from, to, subject, body);

    JS_FreeCString(ctx, from);
    JS_FreeCString(ctx, to);
    JS_FreeCString(ctx, subject);
    JS_FreeCString(ctx, body);

    smtp_do_close(conn);

    if (result != 0) {
        return JS_ThrowTypeError(ctx, "SMTP send failed");
    }

    return JS_TRUE;
}

/* -------------------- Module entry -------------------- */

static int smtp_ext_init(JSContext *ctx, JSValue global) {
    memset(g_conns, 0, sizeof(g_conns));

    JSValue smtp_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, smtp_obj, "connect",
        JS_NewCFunction(ctx, js_smtp_connect, "connect", 4));
    JS_SetPropertyStr(ctx, smtp_obj, "send",
        JS_NewCFunction(ctx, js_smtp_send, "send", 1));
    JS_SetPropertyStr(ctx, global, "smtp", smtp_obj);

    return 0;
}

static int smtp_ext_shutdown(void) {
    for (int i = 0; i < MAX_SMTP_CONNS; i++) {
        if (g_conns[i]) {
            smtp_do_close(g_conns[i]);
            g_conns[i] = NULL;
        }
    }
    return 0;
}

JSCGI_MODULE(smtp, "0.1.0", smtp_ext_init, smtp_ext_shutdown);
