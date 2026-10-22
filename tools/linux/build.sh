#!/bin/sh
# Builds the Linux release in its container (tools/linux/Containerfile's build stage, built here the first time):
# build-linux/ with cmake/linux-i686.cmake, SDL3 with X11, Wayland and its audio, then
# build-linux/crash2-pc-linux.AppImage (tools/linux/appimage.sh). Run from the repository's root, once the
# decompilation is split (README.md, "Building"); podman or docker.
set -eu

IMAGE=crash2-pc-linux
ENGINE=$(command -v podman || command -v docker)

if ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    "$ENGINE" build --target build -t "$IMAGE" -f tools/linux/Containerfile tools/linux
else
    echo "$IMAGE: built already"
fi
# pkg-config finds the 32-bit libraries SDL3 builds against
"$ENGINE" run --rm -v "$PWD":/project -w /project \
    --env PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig "$IMAGE" sh -c '
    set -eu
    cmake -S . -B build-linux -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i686.cmake
    cmake --build build-linux
    sh tools/linux/appimage.sh
'
