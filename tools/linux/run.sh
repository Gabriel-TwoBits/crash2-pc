#!/bin/sh
# Runs build-linux/crash2-pc-linux.AppImage in a 64-bit system with no 32-bit library (tools/linux/Containerfile's
# test stage, built here), for the same checks as the build's own runs (CLAUDE.md, "Running and testing"):
#   tools/linux/run.sh [--64] [--gpu] [--window] DISC.cue OUT [NAME=VALUE ...]
# --64 runs the 64-bit build's, build-linux64/crash2-pc-linux-x86_64.AppImage. Headless, or with --window in a window
# on Xvfb's virtual display; with --gpu in the test-gpu stage, which adds 32-bit Vulkan for the GPU renderer (64-bit
# Vulkan with --64). The disc's folder is mounted read-only at /disc, OUT at /out; each NAME=VALUE
# goes to the program's environment (CRASH2_GAME_CLOCK=/out). The container has no FUSE: the AppImage extracts itself
# and runs.
set -eu

BUILD=build-linux
APPIMAGE=crash2-pc-linux.AppImage
GPU_STAGE=test-gpu
if [ "${1:-}" = "--64" ]; then
    BUILD=build-linux64
    APPIMAGE=crash2-pc-linux-x86_64.AppImage
    GPU_STAGE=test-gpu64
    shift
else
    : # The 32-bit build's
fi
STAGE=test
if [ "${1:-}" = "--gpu" ]; then
    STAGE=$GPU_STAGE
    shift
else
    : # The system with no 32-bit library
fi
if [ "${1:-}" = "--window" ]; then
    LAUNCH="xvfb-run -a"
    HEADLESS=""
    shift
else
    LAUNCH=""
    HEADLESS="--env CRASH2_HEADLESS=1"
fi
if [ $# -lt 2 ]; then
    sed -n '4,10p' "$0"
    exit 2
else
    DISC=$(cd "$(dirname "$1")" && pwd)
    CUE=$(basename "$1")
    OUT=$(mkdir -p "$2" && cd "$2" && pwd)
    shift 2
fi
IMAGE=crash2-pc-linux-$STAGE
ENGINE=$(command -v podman || command -v docker)
# Built each time, so a changed Containerfile takes effect (the layers' cache keeps it quick)
"$ENGINE" build --target "$STAGE" -t "$IMAGE" -f tools/linux/Containerfile tools/linux
ENVIRONMENT=""
for setting in "$@"; do
    ENVIRONMENT="$ENVIRONMENT --env $setting"
done
# --init: as the container's first process, xvfb-run never hears from Xvfb that its display is up
# shellcheck disable=SC2086 # each --env and the launch's words apart
"$ENGINE" run --rm --init -v "$PWD/$BUILD":/build:ro -v "$DISC":/disc:ro -v "$OUT":/out -w /out \
    --env APPIMAGE_EXTRACT_AND_RUN=1 $HEADLESS $ENVIRONMENT "$IMAGE" \
    $LAUNCH "/build/$APPIMAGE" "/disc/$CUE"
