#!/bin/bash
# Notarizes build/CursorLingo.app with Apple and staples the ticket, so Gatekeeper opens it without warnings.
# Build it first with a Developer ID:  SIGN_IDENTITY="Developer ID Application: ..." macos/build.sh
#
# Credentials, either:
#   NOTARY_PROFILE=<name>    a keychain profile, created once with: xcrun notarytool store-credentials <name>
#   ASC_KEY_PATH, ASC_KEY_ID, ASC_ISSUER_ID    an App Store Connect API key (.p8), as used in CI
set -euo pipefail
cd "$(dirname "$0")/../build"
APP=CursorLingo.app
SUBMIT="$(mktemp -d)/CursorLingo-notarize.zip"

if [ -n "${NOTARY_PROFILE:-}" ]; then
  AUTH=(--keychain-profile "$NOTARY_PROFILE")
else
  AUTH=(--key "$ASC_KEY_PATH" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER_ID")
fi

ditto -c -k --keepParent "$APP" "$SUBMIT"
RESULT="$(xcrun notarytool submit "$SUBMIT" "${AUTH[@]}" --wait --timeout 30m --output-format json)"
echo "$RESULT"
STATUS="$(echo "$RESULT" | /usr/bin/plutil -extract status raw -o - - 2>/dev/null || true)"
if [ "$STATUS" != "Accepted" ]; then
  ID="$(echo "$RESULT" | /usr/bin/plutil -extract id raw -o - - 2>/dev/null || true)"
  [ -n "$ID" ] && xcrun notarytool log "$ID" "${AUTH[@]}" || true
  echo "Notarization failed: ${STATUS:-unknown}" >&2
  exit 1
fi

xcrun stapler staple "$APP"
spctl --assess --type execute --verbose=2 "$APP"
echo "Notarized and stapled $APP"
