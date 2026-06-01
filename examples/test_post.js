response.setHeader("Content-Type", "application/json");

const result = {
    method: request.method,
    body: request.body,
    contentType: request.headers["content-type"] || "not set"
};

print(JSON.stringify(result, null, 2));
