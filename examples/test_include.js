include("./includes/header.js");

const name = request.query.name || "World";
print(`<h1>Hello, ${name}!</h1>`);
print("<p>This page uses includes for header and footer.</p>");

include("./includes/footer.js");
