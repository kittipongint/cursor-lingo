#!/bin/bash
set -euo pipefail
APP="$HOME/Applications/CursorLingo.app"
pkill -x CursorLingo 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/io.github.cursorlingo.plist"
defaults delete io.github.cursorlingo 2>/dev/null || true
if [ -d "$APP" ]; then rm -rf "$APP"; echo "Removed $APP"; else echo "CursorLingo is not installed."; fi
echo "If you enabled Launch at Login, also remove CursorLingo from System Settings > General > Login Items."
