#!/bin/bash
set -e

QUICKJS_DIR="quickjs"

# Download QuickJS-ng if not present
if [ ! -d "$QUICKJS_DIR" ]; then
    echo "Downloading QuickJS-ng..."
    git clone --depth 1 https://github.com/quickjs-ng/quickjs.git "$QUICKJS_DIR"
fi

# Build jscgi
echo "Building jscgi..."
make

echo ""
echo "Build complete!"
echo "Binary: $(pwd)/jscgi"
echo ""
echo "To install:"
echo "  sudo cp jscgi /usr/lib/cgi-bin/jscgi"
echo "  sudo mkdir -p /etc/jscgi"
echo "  sudo cp jscgi.ini /etc/jscgi/jscgi.ini"
echo "  sudo mkdir -p /usr/lib/jscgi/modules"
