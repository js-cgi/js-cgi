response.setHeader("Content-Type", "application/json");

// GET request
const res = http.get("https://httpbin.org/get?foo=bar");

// POST request
const postRes = http.post(
    "https://httpbin.org/post",
    JSON.stringify({ message: "Hello from js-cgi" }),
    { "Content-Type": "application/json" }
);

print(JSON.stringify({
    get: {
        status: res.status,
        url: JSON.parse(res.body).url
    },
    post: {
        status: postRes.status,
        data: JSON.parse(postRes.body).data
    }
}, null, 2));
