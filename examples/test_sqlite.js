response.setHeader("Content-Type", "application/json");

const db = sqlite.open("/tmp/test_js-cgi.db");

db.exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, email TEXT)");
db.exec("DELETE FROM users");

db.exec("INSERT INTO users (name, email) VALUES (?, ?)", ["Alice", "alice@example.com"]);
db.exec("INSERT INTO users (name, email) VALUES (?, ?)", ["Bob", "bob@example.com"]);
db.exec("INSERT INTO users (name, email) VALUES (?, ?)", ["Charlie", "charlie@example.com"]);

const all = db.query("SELECT * FROM users");
const one = db.queryOne("SELECT * FROM users WHERE name = ?", ["Bob"]);

db.close();

print(JSON.stringify({ all, one }, null, 2));
