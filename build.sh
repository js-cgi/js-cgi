#!/bin/bash
set -e

QUICKJS_DIR="quickjs"

# Download QuickJS-ng if not present
if [ ! -d "$QUICKJS_DIR" ]; then
    echo "Downloading QuickJS-ng..."
    git clone --depth 1 https://github.com/quickjs-ng/quickjs.git "$QUICKJS_DIR"
fi

# Build js-cgi
echo "Building js-cgi..."
make

echo ""
echo "Build complete!"
echo "Binary: $(pwd)/js-cgi"
echo ""
echo "To install:"
echo "  sudo cp js-cgi /usr/lib/cgi-bin/js-cgi"
echo "  sudo mkdir -p /etc/js-cgi"
echo "  sudo cp js-cgi.ini /etc/js-cgi/js-cgi.ini"
echo "  sudo mkdir -p /usr/lib/js-cgi/modules"
