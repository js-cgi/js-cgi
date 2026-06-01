response.setHeader("Content-Type", "application/json");

session.start();

// Increment visit counter
let visits = session.get("visits");
if (visits === null) {
    visits = 0;
}
visits++;
session.set("visits", visits);

session.set("last_page", request.path);

print(JSON.stringify({
    visits: visits,
    last_page: session.get("last_page")
}, null, 2));
