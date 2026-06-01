import { greet, toUpper } from "./modules/utils.js";

const name = request.query.name || "World";
print(`<h1>${greet(name)}</h1>`);
print(`<p>${toUpper(name)}</p>`);
