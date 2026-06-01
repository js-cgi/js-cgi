response.setHeader("Content-Type", "application/json");

const headers = request.headers;
print(JSON.stringify(headers, null, 2));
