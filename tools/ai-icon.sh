#!/bin/bash
# Optional: regenerate the app icon master with OpenAI's image API, then repackage
# it into .icns / .ico.  Requires OPENAI_API_KEY and jq.
#
#   OPENAI_API_KEY=sk-... ./tools/ai-icon.sh ["custom prompt"]
#
# The default icon is drawn in code (tools/make-icon.swift) and needs no API key.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${OPENAI_API_KEY:?Set OPENAI_API_KEY first}"

PROMPT="${1:-A modern macOS app icon, rounded square with an indigo-to-blue gradient. A crisp white mouse pointer arrow with a thin dark outline, and next to its tip a glossy green pill-shaped badge with white bold text \"Aก\" (Latin A and Thai ko kai). Flat vector style, soft shadows, centered, no other text, transparent background.}"

curl -sS https://api.openai.com/v1/images/generations \
  -H "Authorization: Bearer $OPENAI_API_KEY" \
  -H "Content-Type: application/json" \
  -d "$(jq -n --arg p "$PROMPT" '{model:"gpt-image-1", prompt:$p, size:"1024x1024", background:"transparent", n:1}')" \
  | jq -r '.data[0].b64_json' | base64 --decode > assets/AppIcon.png

swift tools/make-icon.swift --from-png
