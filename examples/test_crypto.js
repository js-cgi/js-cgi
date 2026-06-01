response.setHeader("Content-Type", "application/json");

const result = {
    md5: crypto.md5("hello"),
    sha1: crypto.sha1("hello"),
    sha256: crypto.sha256("hello"),
    sha512: crypto.sha512("hello"),
    hmac: crypto.hmac("sha256", "secret-key", "hello"),
    randomBytes: crypto.randomBytes(16),
    uuid: crypto.uuid()
};

print(JSON.stringify(result, null, 2));
