response.setHeader("Content-Type", "application/json");

const db = sqlite.open("/tmp/test_js-cgi_adv.db");

db.exec("CREATE TABLE IF NOT EXISTS orders (id INTEGER PRIMARY KEY AUTOINCREMENT, product TEXT, qty INTEGER, price REAL)");
db.exec("DELETE FROM orders");

// Test lastInsertId
db.exec("INSERT INTO orders (product, qty, price) VALUES (?, ?, ?)", ["Widget", 5, 9.99]);
const id1 = db.lastInsertId();

db.exec("INSERT INTO orders (product, qty, price) VALUES (?, ?, ?)", ["Gadget", 2, 24.95]);
const id2 = db.lastInsertId();

// Test changes
db.exec("UPDATE orders SET qty = 10 WHERE product = ?", ["Widget"]);
const updated = db.changes();

// Test transactions - commit
db.beginTransaction();
db.exec("INSERT INTO orders (product, qty, price) VALUES (?, ?, ?)", ["Doohickey", 1, 4.50]);
db.commit();

// Test transactions - rollback
db.beginTransaction();
db.exec("INSERT INTO orders (product, qty, price) VALUES (?, ?, ?)", ["Ghost Item", 1, 0.00]);
db.rollback();

const all = db.query("SELECT * FROM orders");
db.close();

print(JSON.stringify({
    lastInsertIds: { first: id1, second: id2 },
    changesAfterUpdate: updated,
    orders: all
}, null, 2));
