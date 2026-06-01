response.setHeader("Content-Type", "application/json");

const db = sqlite.open("/tmp/test_js-cgi_complete.db");

// Test tableExists before creation
const beforeCreate = db.tableExists("items");

// Create table
db.exec("CREATE TABLE IF NOT EXISTS items (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, data BLOB)");

// Test tableExists after creation
const afterCreate = db.tableExists("items");
const nonExistent = db.tableExists("fake_table");

// Test BLOB support
const blobData = [72, 101, 108, 108, 111]; // "Hello" in bytes
db.exec("INSERT INTO items (name, data) VALUES (?, ?)", ["binary test", blobData]);
const row = db.queryOne("SELECT * FROM items WHERE name = ?", ["binary test"]);

// Test error on closed connection
db.close();

let closedError = null;
try {
    db.query("SELECT 1");
} catch (e) {
    closedError = e.message;
}

print(JSON.stringify({
    tableExists: {
        beforeCreate,
        afterCreate,
        nonExistent
    },
    blob: {
        stored: blobData,
        retrieved: row.data
    },
    closedConnectionError: closedError
}, null, 2));
