#!/usr/bin/env bash
# Standalone macOS distribution: builds Release, assembles Infinity.app
# (binary + bundled wgpu-native dylib + icon + Info.plist), ad-hoc signs
# it, and wraps it in a drag-to-Applications DMG. Fully self-contained —
# recipients install nothing else. Apple Silicon (arm64): the bundled
# wgpu-native release library is per-architecture.
#
# Usage: ci/package-mac.sh            (or: cmake --build <dir> --target package-mac)
# Output: build-dist/Infinity-<version>-arm64.dmg
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "package-mac.sh must run on macOS" >&2
  exit 1
fi

BUILD=build-dist
ARCH="$(uname -m)"

echo "=== configure + build (Release) ==="
cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target game_app

VERSION="$(sed -n 's/.*kVersion = "\([^"]*\)".*/\1/p' "$BUILD/game/gen/generated/gen/version.hpp")"
VERSION="${VERSION:-0.0.0}"
APP="$BUILD/Infinity.app"
DMG="$BUILD/Infinity-$VERSION-$ARCH.dmg"

echo "=== assemble Infinity.app (v$VERSION, $ARCH) ==="
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/game/app/infinity" "$APP/Contents/MacOS/infinity"
# The binary carries an @loader_path rpath, so the dylib lives beside it.
cp "$BUILD/game/app/libwgpu_native.dylib" "$APP/Contents/MacOS/"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>               <string>Infinity</string>
  <key>CFBundleDisplayName</key>        <string>Infinity</string>
  <key>CFBundleIdentifier</key>         <string>com.psiori.infinity</string>
  <key>CFBundleExecutable</key>         <string>infinity</string>
  <key>CFBundleIconFile</key>           <string>infinity.icns</string>
  <key>CFBundlePackageType</key>        <string>APPL</string>
  <key>CFBundleShortVersionString</key> <string>$VERSION</string>
  <key>CFBundleVersion</key>            <string>$VERSION</string>
  <key>LSMinimumSystemVersion</key>     <string>12.0</string>
  <key>LSApplicationCategoryType</key>  <string>public.app-category.games</string>
  <key>NSHighResolutionCapable</key>    <true/>
  <key>NSHumanReadableCopyright</key>   <string>© 2026 Sascha Lange</string>
</dict>
</plist>
PLIST

# Icon: the README hero shot, square-cropped through the icns sizes.
if [[ -f docs/deep-sky.png ]]; then
  ICONSET="$BUILD/infinity.iconset"
  rm -rf "$ICONSET"
  mkdir -p "$ICONSET"
  SQ="$BUILD/icon-square.png"
  cp docs/deep-sky.png "$SQ"
  H=$(sips -g pixelHeight "$SQ" | awk '/pixelHeight/{print $2}')
  sips -s format png -c "$H" "$H" "$SQ" --out "$SQ" >/dev/null
  for s in 16 32 128 256 512; do
    sips -z "$s" "$s" "$SQ" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z "$d" "$d" "$SQ" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/infinity.icns"
fi

echo "=== sign (ad-hoc) ==="
codesign --force -s - "$APP/Contents/MacOS/libwgpu_native.dylib"
codesign --force -s - "$APP"
codesign --verify --deep "$APP"

echo "=== smoke: 60 frames from inside the bundle ==="
"$APP/Contents/MacOS/infinity" --windowed --frames 60 >/dev/null

echo "=== DMG ==="
STAGE="$BUILD/dmg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cat > "$STAGE/README.txt" <<'README'
Infinity — a fully procedural universe from a single seed.

Install: drag Infinity.app into Applications (or run it from anywhere).

First launch: this build is not notarized with Apple, so macOS will warn
about an unidentified developer. Right-click Infinity.app and choose
"Open", or approve it under System Settings > Privacy & Security >
"Open Anyway". This is only needed once.

Requires an Apple Silicon Mac (M1 or newer), macOS 12+.

Fly: mouse steers, W thrusts, Shift boosts. E lands / takes off.
M opens the system map. Tap J to target a star along your nose,
hold J to jump to it. Right mouse digs, middle mouse builds.
Cmd+Q quits. Your edits persist per seed in
~/Library/Application Support/Infinity.
README
rm -f "$DMG"
hdiutil create -volname "Infinity" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null

echo "PACKAGED: $DMG"
du -h "$DMG" | cut -f1
