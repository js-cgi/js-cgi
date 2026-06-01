response.setHeader("Content-Type", "application/json");

const testFile = "/tmp/js-cgi-file-test.txt";
const testDir = "/tmp";

// Write a file
file.write(testFile, "Hello from js-cgi!\nLine 2.");

// Check it exists
const exists = file.exists(testFile);

// Read it back
const content = file.read(testFile);

// Get size
const size = file.size(testFile);

// Append to it
file.append(testFile, "\nLine 3.");
const afterAppend = file.read(testFile);

// List directory
const files = file.list("/tmp").filter(f => f.startsWith("js-cgi"));

// Check isDir
const isDir = file.isDir(testDir);
const isNotDir = file.isDir(testFile);

// Delete
file.delete(testFile);
const existsAfterDelete = file.exists(testFile);

print(JSON.stringify({
    exists,
    content,
    size,
    afterAppend,
    files,
    isDir,
    isNotDir,
    existsAfterDelete
}, null, 2));
