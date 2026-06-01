response.setHeader("Content-Type", "application/json");

// Set some cookies
response.setCookie("theme", "dark", { maxAge: 86400, httpOnly: true });
response.setCookie("lang", "en", { path: "/", sameSite: "Lax" });

// Read cookies from request
const cookies = request.cookies;

print(JSON.stringify({
    received_cookies: cookies,
    set_cookies: ["theme=dark", "lang=en"]
}, null, 2));
