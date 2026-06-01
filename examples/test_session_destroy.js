response.setHeader("Content-Type", "application/json");

session.start();
session.destroy();

print(JSON.stringify({ destroyed: true }));
