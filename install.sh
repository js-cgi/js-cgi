#!/bin/bash
set -e

DOWNLOAD_BASE="https://js-cgi.com"
INSTALL_BIN="/usr/lib/cgi-bin/js-cgi"
INSTALL_MODULES="/usr/lib/js-cgi/modules"
INSTALL_CONFIG="/etc/js-cgi"

# Detect architecture
ARCH=$(uname -m)
case "$ARCH" in
    x86_64|amd64)
        ARCHIVE="latest-x86_64.tar.gz"
        ;;
    aarch64|arm64)
        ARCHIVE="latest-arm64.tar.gz"
        ;;
    *)
        echo "Error: Unsupported architecture: $ARCH"
        exit 1
        ;;
esac

echo "js-cgi installer"
echo "================="
echo ""
echo "Architecture: $ARCH"
echo "Downloading:  $DOWNLOAD_BASE/$ARCHIVE"
echo ""

# Check for root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root (use sudo)"
    exit 1
fi

# Create temp directory
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Download
echo "Downloading js-cgi..."
curl -sSL "$DOWNLOAD_BASE/$ARCHIVE" -o "$TMP/js-cgi.tar.gz"

# Extract
echo "Extracting..."
tar -xzf "$TMP/js-cgi.tar.gz" -C "$TMP"

# Install binary
echo "Installing binary..."
cp "$TMP/js-cgi/js-cgi" "$INSTALL_BIN"
chmod 755 "$INSTALL_BIN"

# Install modules
echo "Installing extensions..."
mkdir -p "$INSTALL_MODULES"
cp "$TMP/js-cgi/modules/"*.so "$INSTALL_MODULES/"

# Install config (don't overwrite existing)
mkdir -p "$INSTALL_CONFIG"
if [ ! -f "$INSTALL_CONFIG/js-cgi.ini" ]; then
    echo "Installing default configuration..."
    cp "$TMP/js-cgi/js-cgi.ini" "$INSTALL_CONFIG/js-cgi.ini"
else
    echo "Configuration already exists, skipping."
fi

echo ""
echo "Installation complete!"
echo ""
echo "  Binary:     $INSTALL_BIN"
echo "  Extensions: $INSTALL_MODULES"
echo "  Config:     $INSTALL_CONFIG/js-cgi.ini"
echo ""
echo "Next steps:"
echo "  1. Enable Apache CGI: sudo a2enmod cgid actions"
echo "  2. Configure your virtual host (see https://js-cgi.com/docs/getting-started.js)"
echo "  3. Create /var/www/js/index.js and start building"
echo ""
