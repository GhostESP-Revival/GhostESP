#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "Building WebUI bundle..."
node "$SCRIPT_DIR/build.mjs"

echo "Generating embedded WebUI header..."
python3 "$SCRIPT_DIR/html_to_header.py"

if [ -f "$PROJECT_ROOT/include/managers/ghost_esp_site_gz.h" ]; then
    echo "Generated $PROJECT_ROOT/include/managers/ghost_esp_site_gz.h"
    ls -lh "$PROJECT_ROOT/include/managers/ghost_esp_site_gz.h"
else
    echo "Header file was not generated"
    exit 1
fi
