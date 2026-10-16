#!/bin/bash
# Builds CursorLingo from source and installs it into ~/Applications (macOS).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
"$ROOT/macos/build.sh"
DEST="$HOME/Applications/CursorLingo.app"
pkill -x CursorLingo 2>/dev/null || true
mkdir -p "$HOME/Applications"
rm -rf "$DEST"
cp -R "$ROOT/build/CursorLingo.app" "$DEST"
echo "Installed: $DEST"
open "$DEST"
