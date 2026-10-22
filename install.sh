#!/bin/bash
# Builds CursorLingo from source and installs it into ~/Applications (macOS).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
# Sign with your Developer ID if you have one, so macOS keeps CursorLingo's permissions across rebuilds.
if [ -z "${SIGN_IDENTITY:-}" ]; then
  SIGN_IDENTITY="$(security find-identity -v -p codesigning 2>/dev/null \
    | sed -n 's/.*"\(Developer ID Application:[^"]*\)".*/\1/p' | head -1)"
  export SIGN_IDENTITY="${SIGN_IDENTITY:--}"
fi
"$ROOT/macos/build.sh"
DEST="$HOME/Applications/CursorLingo.app"
pkill -x CursorLingo 2>/dev/null || true
mkdir -p "$HOME/Applications"
rm -rf "$DEST"
cp -R "$ROOT/build/CursorLingo.app" "$DEST"
echo "Installed: $DEST"
open "$DEST"
