#!/bin/bash
# Builds a universal (Apple Silicon + Intel, macOS 11+) StageRack.app and zips it
# for copying to other Macs.  Usage:  scripts/package.sh [codesign identity]
#   no argument  -> ad-hoc signature (on the other Mac: right click > Open the first time,
#                   or System Settings > Privacy & Security > "Open Anyway")
#   "Developer ID Application: ..." -> real signature (then notarize to avoid any warning)
set -euo pipefail
cd "$(dirname "$0")/.."

IDENTITY="${1:--}"
BUILD=build
APP="$BUILD/StageRack_artefacts/Release/StageRack.app"
DIST=dist

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 > /dev/null
cmake --build "$BUILD"

rm -rf "$DIST" && mkdir -p "$DIST"
ditto "$APP" "$DIST/StageRack.app"

# Seal the whole bundle (the linker signature alone does not cover Info.plist/resources,
# and a quarantined copy could then be reported as "damaged").
# No hardened runtime: it would block loading third-party plugins without extra entitlements.
codesign --force --deep --sign "$IDENTITY" "$DIST/StageRack.app"
codesign --verify --deep --strict "$DIST/StageRack.app"

VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" "$DIST/StageRack.app/Contents/Info.plist")
ZIP="$DIST/StageRack-$VERSION-macOS11-universal.zip"
ditto -c -k --keepParent "$DIST/StageRack.app" "$ZIP"

lipo -archs "$DIST/StageRack.app/Contents/MacOS/StageRack"
echo "OK -> $ZIP"
