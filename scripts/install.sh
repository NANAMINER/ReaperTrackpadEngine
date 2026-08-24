#!/usr/bin/env bash
# Copies the built extension into REAPER's UserPlugins folder.
# Does NOT touch a running REAPER instance -- you still need to (re)start
# REAPER for it to pick up the new/changed dylib.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DYLIB="$PROJECT_ROOT/build/reaper_trackpadengine.dylib"
DEST_DIR="$HOME/Library/Application Support/REAPER/UserPlugins"

if [ ! -f "$DYLIB" ]; then
    echo "error: $DYLIB not found. Build first:" >&2
    echo "  cmake -S \"$PROJECT_ROOT\" -B \"$PROJECT_ROOT/build\"" >&2
    echo "  cmake --build \"$PROJECT_ROOT/build\"" >&2
    exit 1
fi

mkdir -p "$DEST_DIR"
cp "$DYLIB" "$DEST_DIR/reaper_trackpadengine.dylib"

echo "Installed: $DEST_DIR/reaper_trackpadengine.dylib"
echo "Restart REAPER (fully quit and reopen) to load it."
