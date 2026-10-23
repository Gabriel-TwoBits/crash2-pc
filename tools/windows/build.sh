#!/bin/sh
# Builds the Windows programs in the Windows build's container (tools/windows/Containerfile, built here) with
# MinGW-w64's GCC: the 32-bit build through cmake/mingw-i686.cmake into build-windows/ (crash2.exe, which loads
# crash2-game.dll), the 64-bit build through cmake/mingw-x86_64.cmake into build-windows64/ (crash2.exe), each with
# SDL3.dll beside it. Then each build's zip, build-windows/crash2-pc-windows.zip and
# build-windows64/crash2-pc-windows-x86_64.zip: the programs stripped of their debugging symbols, with the licences
# (the port's and SwanStation's GPL-3.0, SDL's zlib licence, and the 64-bit build's wasm2c runtime and C library) and
# the README. Run from the repository's root, once the decompilation is split (README.md, "Building"); podman or
# docker.
set -eu

IMAGE=crash2-pc-windows
ENGINE=$(command -v podman || command -v docker)

# Built each time, so a changed Containerfile takes effect (the layers' cache keeps it quick)
"$ENGINE" build -t "$IMAGE" -f tools/windows/Containerfile tools/windows
"$ENGINE" run --rm -v "$PWD":/project -w /project "$IMAGE" sh -c '
    set -eu
    # BUILD TOOLCHAIN ZIP TOOLS_PREFIX
    package() {
        cmake -S . -B "$1" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$2"
        cmake --build "$1"
        PACKAGE="$1"/package/crash2-pc
        rm -rf "$1"/package "$1/$3"
        mkdir -p "$PACKAGE"
        cp "$1"/crash2.exe "$1"/*.dll "$PACKAGE"
        "$4"-strip --strip-debug "$PACKAGE"/crash2.exe "$PACKAGE"/*.dll
        cp LICENSE "$PACKAGE"/LICENSE.txt
        cp third_party/swanstation/LICENSE "$PACKAGE"/LICENSE-SwanStation.txt
        cp "$1"/_deps/sdl3-src/LICENSE.txt "$PACKAGE"/LICENSE-SDL3.txt
        if [ -d "$1"/licenses ]; then
            cp "$1"/licenses/LICENSE-*.txt "$PACKAGE"
        else
            : # The 32-bit build carries no other library
        fi
        cp README.md "$PACKAGE"/README.md
        (cd "$1"/package && python3 -m zipfile -c "../$3" crash2-pc)
        echo "$1/$3"
    }
    package build-windows cmake/mingw-i686.cmake crash2-pc-windows.zip i686-w64-mingw32
    package build-windows64 cmake/mingw-x86_64.cmake crash2-pc-windows-x86_64.zip x86_64-w64-mingw32
'
