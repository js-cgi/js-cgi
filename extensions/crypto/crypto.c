#include "../../js-cgi-module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>

/* -------------------- Helpers -------------------- */

static void bytes_to_hex(const unsigned char *bytes, size_t len, char *hex) {
    for (size_t i = 0; i < len; i++) {
        sprintf(hex + (i * 2), "%02x", bytes[i]);
    }
    hex[len * 2] = '\0';
}

/* -------------------- JS API: crypto.md5() -------------------- */

static JSValue js_crypto_md5(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.md5 requires a string");

    size_t input_len;
    const char *input = JS_ToCStringLen(ctx, &input_len, argv[0]);
    if (!input) return JS_EXCEPTION;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_md5(), NULL);
    EVP_DigestUpdate(mdctx, input, input_len);
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    JS_FreeCString(ctx, input);

    char hex[65];
    bytes_to_hex(hash, hash_len, hex);
    return JS_NewString(ctx, hex);
}

/* -------------------- JS API: crypto.sha1() -------------------- */

static JSValue js_crypto_sha1(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.sha1 requires a string");

    size_t input_len;
    const char *input = JS_ToCStringLen(ctx, &input_len, argv[0]);
    if (!input) return JS_EXCEPTION;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(mdctx, input, input_len);
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    JS_FreeCString(ctx, input);

    char hex[65];
    bytes_to_hex(hash, hash_len, hex);
    return JS_NewString(ctx, hex);
}

/* -------------------- JS API: crypto.sha256() -------------------- */

static JSValue js_crypto_sha256(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.sha256 requires a string");

    size_t input_len;
    const char *input = JS_ToCStringLen(ctx, &input_len, argv[0]);
    if (!input) return JS_EXCEPTION;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(mdctx, input, input_len);
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    JS_FreeCString(ctx, input);

    char hex[65];
    bytes_to_hex(hash, hash_len, hex);
    return JS_NewString(ctx, hex);
}

/* -------------------- JS API: crypto.sha512() -------------------- */

static JSValue js_crypto_sha512(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.sha512 requires a string");

    size_t input_len;
    const char *input = JS_ToCStringLen(ctx, &input_len, argv[0]);
    if (!input) return JS_EXCEPTION;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha512(), NULL);
    EVP_DigestUpdate(mdctx, input, input_len);
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    JS_FreeCString(ctx, input);

    char hex[129];
    bytes_to_hex(hash, hash_len, hex);
    return JS_NewString(ctx, hex);
}

/* -------------------- JS API: crypto.hmac() -------------------- */

static JSValue js_crypto_hmac(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "crypto.hmac requires algorithm, key, and data");

    const char *algo = JS_ToCString(ctx, argv[0]);
    if (!algo) return JS_EXCEPTION;

    size_t key_len;
    const char *key = JS_ToCStringLen(ctx, &key_len, argv[1]);
    if (!key) { JS_FreeCString(ctx, algo); return JS_EXCEPTION; }

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[2]);
    if (!data) { JS_FreeCString(ctx, algo); JS_FreeCString(ctx, key); return JS_EXCEPTION; }

    const EVP_MD *md = NULL;
    if (strcasecmp(algo, "sha256") == 0) md = EVP_sha256();
    else if (strcasecmp(algo, "sha1") == 0) md = EVP_sha1();
    else if (strcasecmp(algo, "sha512") == 0) md = EVP_sha512();
    else if (strcasecmp(algo, "md5") == 0) md = EVP_md5();

    JS_FreeCString(ctx, algo);

    if (!md) {
        JS_FreeCString(ctx, key);
        JS_FreeCString(ctx, data);
        return JS_ThrowTypeError(ctx, "Unsupported algorithm. Use: md5, sha1, sha256, sha512");
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    HMAC(md, key, key_len, (unsigned char *)data, data_len, hash, &hash_len);

    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, data);

    char hex[129];
    bytes_to_hex(hash, hash_len, hex);
    return JS_NewString(ctx, hex);
}

/* -------------------- JS API: crypto.hmacHex() -------------------- */

static int hex_char_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static JSValue js_crypto_hmac_hex(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "crypto.hmacHex requires algorithm, hexKey, and data");

    const char *algo = JS_ToCString(ctx, argv[0]);
    if (!algo) return JS_EXCEPTION;

    const char *hex_key = JS_ToCString(ctx, argv[1]);
    if (!hex_key) { JS_FreeCString(ctx, algo); return JS_EXCEPTION; }

    size_t data_len;
    const char *data = JS_ToCStringLen(ctx, &data_len, argv[2]);
    if (!data) { JS_FreeCString(ctx, algo); JS_FreeCString(ctx, hex_key); return JS_EXCEPTION; }

    const EVP_MD *md = NULL;
    if (strcasecmp(algo, "sha256") == 0) md = EVP_sha256();
    else if (strcasecmp(algo, "sha1") == 0) md = EVP_sha1();
    else if (strcasecmp(algo, "sha512") == 0) md = EVP_sha512();
    else if (strcasecmp(algo, "md5") == 0) md = EVP_md5();

    JS_FreeCString(ctx, algo);

    if (!md) {
        JS_FreeCString(ctx, hex_key);
        JS_FreeCString(ctx, data);
        return JS_ThrowTypeError(ctx, "Unsupported algorithm. Use: md5, sha1, sha256, sha512");
    }

    size_t hex_len = strlen(hex_key);
    size_t key_len = hex_len / 2;
    unsigned char *key_bytes = malloc(key_len);

    for (size_t i = 0; i < key_len; i++) {
        int hi = hex_char_to_int(hex_key[i * 2]);
        int lo = hex_char_to_int(hex_key[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            free(key_bytes);
            JS_FreeCString(ctx, hex_key);
            JS_FreeCString(ctx, data);
            return JS_ThrowTypeError(ctx, "Invalid hex character in key");
        }
        key_bytes[i] = (unsigned char)((hi << 4) | lo);
    }

    JS_FreeCString(ctx, hex_key);

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    HMAC(md, key_bytes, key_len, (unsigned char *)data, data_len, hash, &hash_len);

    free(key_bytes);
    JS_FreeCString(ctx, data);

    char hex[129];
    bytes_to_hex(hash, hash_len, hex);
    return JS_NewString(ctx, hex);
}

/* -------------------- JS API: crypto.randomBytes() -------------------- */

static JSValue js_crypto_random_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "crypto.randomBytes requires a length");

    int64_t len;
    JS_ToInt64(ctx, &len, argv[0]);

    if (len <= 0 || len > 65536) {
        return JS_ThrowTypeError(ctx, "Length must be between 1 and 65536");
    }

    unsigned char *buf = malloc(len);
    RAND_bytes(buf, len);

    char *hex = malloc(len * 2 + 1);
    bytes_to_hex(buf, len, hex);
    free(buf);

    JSValue result = JS_NewString(ctx, hex);
    free(hex);
    return result;
}

/* -------------------- JS API: crypto.uuid() -------------------- */

static JSValue js_crypto_uuid(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    unsigned char bytes[16];
    RAND_bytes(bytes, 16);

    /* Set version 4 */
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    /* Set variant */
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    char uuid[37];
    sprintf(uuid, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);

    return JS_NewString(ctx, uuid);
}

/* -------------------- Module entry -------------------- */

static int crypto_init(JSContext *ctx, JSValue global) {
    JSValue crypto_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto_obj, "md5",
        JS_NewCFunction(ctx, js_crypto_md5, "md5", 1));
    JS_SetPropertyStr(ctx, crypto_obj, "sha1",
        JS_NewCFunction(ctx, js_crypto_sha1, "sha1", 1));
    JS_SetPropertyStr(ctx, crypto_obj, "sha256",
        JS_NewCFunction(ctx, js_crypto_sha256, "sha256", 1));
    JS_SetPropertyStr(ctx, crypto_obj, "sha512",
        JS_NewCFunction(ctx, js_crypto_sha512, "sha512", 1));
    JS_SetPropertyStr(ctx, crypto_obj, "hmac",
        JS_NewCFunction(ctx, js_crypto_hmac, "hmac", 3));
    JS_SetPropertyStr(ctx, crypto_obj, "hmacHex",
        JS_NewCFunction(ctx, js_crypto_hmac_hex, "hmacHex", 3));
    JS_SetPropertyStr(ctx, crypto_obj, "randomBytes",
        JS_NewCFunction(ctx, js_crypto_random_bytes, "randomBytes", 1));
    JS_SetPropertyStr(ctx, crypto_obj, "uuid",
        JS_NewCFunction(ctx, js_crypto_uuid, "uuid", 0));
    JS_SetPropertyStr(ctx, global, "crypto", crypto_obj);

    return 0;
}

static int crypto_shutdown(void) {
    return 0;
}

JSCGI_MODULE(crypto, "0.1.0", crypto_init, crypto_shutdown);
