#!/bin/sh
# On macOS, after the 64-bit build (cmake/host64.cmake): BUILD/crash2-pc.app, the program with SDL3's library in its
# Frameworks and the licences in its Resources, signed ad hoc (without an Apple Developer ID, macOS asks the player to
# allow it once), then BUILD/crash2-pc-macos.zip.
#   sh tools/macos/bundle.sh BUILD VERSION
set -eu

if [ $# -ne 2 ]; then
    sed -n '2,5p' "$0"
    exit 2
else
    BUILD=$1
    VERSION=$2
fi
APP=$BUILD/crash2-pc.app
CONTENTS=$APP/Contents
# The oldest macOS the build runs on, as it was configured
MINIMUM=$(sed -n 's/^CMAKE_OSX_DEPLOYMENT_TARGET:[A-Z]*=//p' "$BUILD/CMakeCache.txt")

rm -rf "$APP" "$BUILD/crash2-pc-macos.zip"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Frameworks" "$CONTENTS/Resources"
cp "$BUILD/crash2" "$CONTENTS/MacOS/crash2"
cp "$BUILD"/libSDL3.*.dylib "$CONTENTS/Frameworks/"
# Their debugging symbols left out
strip -S "$CONTENTS/MacOS/crash2" "$CONTENTS"/Frameworks/*.dylib
cp LICENSE "$CONTENTS/Resources/LICENSE.txt"
cp third_party/swanstation/LICENSE "$CONTENTS/Resources/LICENSE-SwanStation.txt"
cp "$BUILD/_deps/sdl3-src/LICENSE.txt" "$CONTENTS/Resources/LICENSE-SDL3.txt"
cp "$BUILD"/licenses/LICENSE-*.txt "$CONTENTS/Resources/"
cat > "$CONTENTS/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>crash2</string>
    <key>CFBundleIdentifier</key>
    <string>io.github.gabriel-twobits.crash2-pc</string>
    <key>CFBundleName</key>
    <string>crash2-pc</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$VERSION</string>
    <key>CFBundleVersion</key>
    <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>
    <string>$MINIMUM</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.arcade-games</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST
# The library first, then the app around it
codesign --force --sign - "$CONTENTS"/Frameworks/*.dylib
codesign --force --sign - "$APP"
codesign --verify --strict "$APP"
ditto -c -k --keepParent "$APP" "$BUILD/crash2-pc-macos.zip"
echo "$BUILD/crash2-pc-macos.zip"
