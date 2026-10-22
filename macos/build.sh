#!/bin/bash
# Builds a universal (arm64 + x86_64) CursorLingo.app into ../build/
set -euo pipefail
cd "$(dirname "$0")"
OUT="../build/CursorLingo.app"
rm -rf "$OUT"
mkdir -p "$OUT/Contents/MacOS" "$OUT/Contents/Resources"

for arch in arm64 x86_64; do
  # Wrong-layout detector (shared with Windows) is C++ behind a C interface.
  clang++ -std=c++17 -Os -fno-exceptions -fno-rtti -target "$arch-apple-macos12.0" \
    -c ../core/layoutfix_c.cpp -o "../build/layoutfix-$arch.o"
  swiftc -O -whole-module-optimization -target "$arch-apple-macos12.0" \
    -import-objc-header ../core/layoutfix_c.h \
    CursorLingo.swift "../build/layoutfix-$arch.o" -o "../build/CursorLingo-$arch"
done
lipo -create ../build/CursorLingo-arm64 ../build/CursorLingo-x86_64 -output "$OUT/Contents/MacOS/CursorLingo"
rm ../build/CursorLingo-arm64 ../build/CursorLingo-x86_64 ../build/layoutfix-*.o
strip -x "$OUT/Contents/MacOS/CursorLingo"

cp Info.plist "$OUT/Contents/Info.plist"
[ -f ../assets/AppIcon.icns ] && cp ../assets/AppIcon.icns "$OUT/Contents/Resources/AppIcon.icns"

codesign --force --sign - "$OUT"   # ad-hoc signature
echo "Built $OUT"
