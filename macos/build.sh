#!/bin/bash
# Builds a universal (arm64 + x86_64) CursorLingo.app into ../build/
set -euo pipefail
cd "$(dirname "$0")"
OUT="../build/CursorLingo.app"
rm -rf "$OUT"
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"

for arch in arm64 x86_64; do
  swiftc -O -whole-module-optimization -target "$arch-apple-macos12.0" \
    CursorLingo.swift -o "../build/CursorLingo-$arch"
done
lipo -create ../build/CursorLingo-arm64 ../build/CursorLingo-x86_64 -output "$OUT/Contents/MacOS/CursorLingo"
rm ../build/CursorLingo-arm64 ../build/CursorLingo-x86_64
strip -x "$OUT/Contents/MacOS/CursorLingo"

cp Info.plist "$OUT/Contents/Info.plist"
[ -f ../assets/AppIcon.icns ] && cp ../assets/AppIcon.icns "$OUT/Contents/Resources/AppIcon.icns"

codesign --force --sign - "$OUT"   # ad-hoc signature
echo "Built $OUT"
