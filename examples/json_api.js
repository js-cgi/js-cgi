response.setHeader("Content-Type", "application/json");

const data = {
    message: "Hello from js-cgi",
    method: request.method,
    path: request.path,
    query: request.query
};

print(JSON.stringify(data, null, 2));
