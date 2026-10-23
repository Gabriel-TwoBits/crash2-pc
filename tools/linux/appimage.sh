#!/bin/sh
# Inside the Linux release's container (tools/linux/build.sh): a build into its AppImage.
#   sh tools/linux/appimage.sh [BUILD i386|x86_64 APPIMAGE]
# By default the 32-bit build, build-linux/ into build-linux/crash2-pc-linux.AppImage. A 64-bit system rarely has a
# 32-bit program's libraries, its C library included, and the C library differs from one system to another: the
# AppImage carries every library the program loads, from Debian, and AppRun starts it on the C library's own loader
# with them, the 64-bit build's too. SDL3 draws in software where it finds no OpenGL, so no graphics driver is
# carried. The runtime, AppImage's type 2 for x86-64 (the 64-bit system the game needs, docs/ROADMAP.md), is pinned by
# SHA-256.
set -eu

RUNTIME_URL=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
RUNTIME_SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
BUILD=${1:-build-linux}
ARCHITECTURE=${2:-i386}
APPIMAGE=${3:-$BUILD/crash2-pc-linux.AppImage}
APPDIR=$BUILD/AppDir
LIBRARIES=$APPDIR/usr/lib
case "$ARCHITECTURE" in
    i386) SYSTEM_LIBRARIES=/usr/lib/i386-linux-gnu LOADER=ld-linux.so.2 ;;
    x86_64) SYSTEM_LIBRARIES=/usr/lib/x86_64-linux-gnu LOADER=ld-linux-x86-64.so.2 ;;
    *) echo "$ARCHITECTURE: an AppImage for i386 or x86_64" >&2; exit 2 ;;
esac
# The libraries SDL3 loads when it runs (its X11, Wayland, audio and udev support), as its build configured them
SDL_CONFIG=$(ls "$BUILD"/_deps/sdl3-build/include-config-*/build_config/SDL_build_config.h)
LOADED=$(sed -n 's/^#define SDL_[A-Z0-9_]*_DYNAMIC[A-Z0-9_]* "\(.*\)"$/\1/p' "$SDL_CONFIG")
ICON_SIZE=256

rm -rf "$APPDIR" "$APPIMAGE"
mkdir -p "$APPDIR/usr/bin" "$LIBRARIES"
cp "$BUILD/crash2" "$APPDIR/usr/bin/crash2"
cp -L "$BUILD/_deps/sdl3-build/libSDL3.so.0" "$LIBRARIES/"
for library in $LOADED; do
    cp -L "$SYSTEM_LIBRARIES/$library" "$LIBRARIES/"
done
# The program's and SDL3's debugging symbols left out, their symbol tables kept
strip --strip-debug "$APPDIR/usr/bin/crash2" "$LIBRARIES/libSDL3.so.0"
# The licences: the port's and SwanStation's GPL-3.0, SDL's zlib licence, and the 64-bit build's wasm2c runtime and C
# library (cmake/host64.cmake)
DOCUMENTS=$APPDIR/usr/share/doc/crash2-pc
mkdir -p "$DOCUMENTS"
cp LICENSE "$DOCUMENTS/LICENSE.txt"
cp third_party/swanstation/LICENSE "$DOCUMENTS/LICENSE-SwanStation.txt"
cp "$BUILD/_deps/sdl3-src/LICENSE.txt" "$DOCUMENTS/LICENSE-SDL3.txt"
if [ -d "$BUILD/licenses" ]; then
    cp "$BUILD"/licenses/LICENSE-*.txt "$DOCUMENTS"
else
    : # The 32-bit build carries no other library of its own
fi

# Every library those load, the C library's loader among them, by the names they're asked for
for object in "$APPDIR/usr/bin/crash2" "$LIBRARIES"/*; do
    ldd "$object" | sed -n "s/^\s*\(\S*\) => \(\/\S*\) .*/\1 \2/p; s/^\s*\(\/\S*ld-linux\S*\) .*/$LOADER \1/p"
done | sort -u | while read -r name path; do
    cp -L "$path" "$LIBRARIES/$name"
done
for object in "$LIBRARIES"/*; do
    if ldd "$object" 2>/dev/null | grep -q "not found"; then
        echo "$object: a library not found" >&2
        exit 1
    else
        continue
    fi
done

cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/sh
# The game on the libraries the AppImage carries, its C library's loader among them
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/lib/LOADER" --library-path "$HERE/usr/lib" "$HERE/usr/bin/crash2" "$@"
APPRUN
sed -i "s/LOADER/$LOADER/" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
cat > "$APPDIR/crash2-pc.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=crash2-pc
Comment=Crash Bandicoot 2 from your own disc image, a fan port of its decompilation
Exec=crash2
Icon=crash2-pc
Categories=Game;
Terminal=false
DESKTOP
# The icon: a plain orange disc on black, nothing of the game's own art
python3 - "$APPDIR/crash2-pc.png" "$ICON_SIZE" <<'ICON'
import struct, sys, zlib
ORANGE = bytes((0xF0, 0x80, 0x10, 0xFF))
BLACK = bytes((0, 0, 0, 0xFF))
RADIUS_FRACTION = 0.42
PIXEL_CENTRE = 0.5
NO_FILTER = 0
# PNG's IHDR: 8 bits a channel, RGBA (colour type 6), deflate, no filter method, no interlace
BIT_DEPTH, RGBA, DEFLATE, FILTER_METHOD, NO_INTERLACE = 8, 6, 0, 0, 0
path, size = sys.argv[1], int(sys.argv[2])
center, radius = size / 2, size * RADIUS_FRACTION
rows = []
for y in range(size):
    row = bytearray([NO_FILTER])
    for x in range(size):
        inside = (x + PIXEL_CENTRE - center) ** 2 + (y + PIXEL_CENTRE - center) ** 2 <= radius ** 2
        row += ORANGE if inside else BLACK
    rows.append(bytes(row))
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
header = struct.pack(">IIBBBBB", size, size, BIT_DEPTH, RGBA, DEFLATE, FILTER_METHOD, NO_INTERLACE)
open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(b"".join(rows)))
                       + chunk(b"IEND", b""))
ICON
ln -sf crash2-pc.png "$APPDIR/.DirIcon"

curl -sSfL -o "$BUILD/runtime" "$RUNTIME_URL"
if [ "$(sha256sum "$BUILD/runtime" | cut -d' ' -f1)" != "$RUNTIME_SHA256" ]; then
    echo "$RUNTIME_URL: not the pinned runtime" >&2
    exit 1
else
    mksquashfs "$APPDIR" "$BUILD/AppDir.squashfs" -root-owned -noappend -comp zstd -quiet
fi
cat "$BUILD/runtime" "$BUILD/AppDir.squashfs" > "$APPIMAGE"
chmod +x "$APPIMAGE"
rm "$BUILD/runtime" "$BUILD/AppDir.squashfs"
echo "$APPIMAGE"
