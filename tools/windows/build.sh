#!/bin/sh
# Builds the Windows program in the Windows build's container (tools/windows/Containerfile, built here the first
# time): MinGW-w64's 32-bit GCC through cmake/mingw-i686.cmake, into build-windows/ (crash2.exe, which loads
# crash2-game.dll, with SDL3.dll beside them), then build-windows/crash2-pc-windows.zip: the three stripped of their
# debugging symbols, with the licences (the port's and SwanStation's GPL-3.0, SDL's zlib licence) and the README.
# Run from the repository's root, once the decompilation is split (README.md, "Building"); podman or docker.
set -eu

IMAGE=crash2-pc-windows
ENGINE=$(command -v podman || command -v docker)

if ! "$ENGINE" image exists "$IMAGE" 2>/dev/null && ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    "$ENGINE" build -t "$IMAGE" -f tools/windows/Containerfile tools/windows
else
    echo "$IMAGE: built already"
fi
"$ENGINE" run --rm -v "$PWD":/project -w /project "$IMAGE" sh -c '
    set -eu
    cmake -S . -B build-windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake
    cmake --build build-windows
    PACKAGE=build-windows/package/crash2-pc
    rm -rf build-windows/package build-windows/crash2-pc-windows.zip
    mkdir -p "$PACKAGE"
    cp build-windows/crash2.exe build-windows/crash2-game.dll build-windows/SDL3.dll "$PACKAGE"
    i686-w64-mingw32-strip --strip-debug "$PACKAGE"/crash2.exe "$PACKAGE"/crash2-game.dll "$PACKAGE"/SDL3.dll
    cp LICENSE "$PACKAGE"/LICENSE.txt
    cp third_party/swanstation/LICENSE "$PACKAGE"/LICENSE-SwanStation.txt
    cp build-windows/_deps/sdl3-src/LICENSE.txt "$PACKAGE"/LICENSE-SDL3.txt
    cp README.md "$PACKAGE"/README.md
    cd build-windows/package
    python3 -m zipfile -c ../crash2-pc-windows.zip crash2-pc
    echo "build-windows/crash2-pc-windows.zip"
'

