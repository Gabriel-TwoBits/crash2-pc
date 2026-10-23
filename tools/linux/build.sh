#!/bin/sh
# Builds the Linux release in its container (tools/linux/Containerfile's build stage, built here):
# build-linux/ with cmake/linux-i686.cmake, SDL3 with X11, Wayland and its audio, then
# build-linux/crash2-pc-linux.AppImage (tools/linux/appimage.sh); the 64-bit build the same way into build-linux64/
# and build-linux64/crash2-pc-linux-x86_64.AppImage. Run from the repository's root, once the decompilation is split
# (README.md, "Building"); podman or docker.
set -eu

IMAGE=crash2-pc-linux
ENGINE=$(command -v podman || command -v docker)

# Built each time, so a changed Containerfile takes effect (the layers' cache keeps it quick)
"$ENGINE" build --target build -t "$IMAGE" -f tools/linux/Containerfile tools/linux
# pkg-config finds the 32-bit libraries SDL3 builds against
"$ENGINE" run --rm -v "$PWD":/project -w /project \
    --env PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig "$IMAGE" sh -c '
    set -eu
    cmake -S . -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i686.cmake
    cmake --build build-linux
    sh tools/linux/appimage.sh
    PKG_CONFIG_LIBDIR=/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig cmake -S . -B build-linux64 -G Ninja
    cmake --build build-linux64
    sh tools/linux/appimage.sh build-linux64 x86_64 build-linux64/crash2-pc-linux-x86_64.AppImage
'
