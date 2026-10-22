#!/bin/sh
# Runs build-linux/crash2-pc-linux.AppImage in a 64-bit system with no 32-bit library (tools/linux/Containerfile's
# test stage, built here the first time), for the same checks as the build's own runs (CLAUDE.md, "Running and
# testing"):
#   tools/linux/run.sh [--window] DISC.cue OUT [NAME=VALUE ...]
# Headless, or with --window in a window on Xvfb's virtual display. The disc's folder is mounted read-only at /disc,
# OUT at /out; each NAME=VALUE goes to the program's environment (CRASH2_GAME_CLOCK=/out). The container has no FUSE:
# the AppImage extracts itself and runs.
set -eu

if [ "${1:-}" = "--window" ]; then
    LAUNCH="xvfb-run -a"
    HEADLESS=""
    shift
else
    LAUNCH=""
    HEADLESS="--env CRASH2_HEADLESS=1"
fi
if [ $# -lt 2 ]; then
    sed -n '5,9p' "$0"
    exit 2
else
    DISC=$(cd "$(dirname "$1")" && pwd)
    CUE=$(basename "$1")
    OUT=$(mkdir -p "$2" && cd "$2" && pwd)
    shift 2
fi
IMAGE=crash2-pc-linux-test
ENGINE=$(command -v podman || command -v docker)
if ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    "$ENGINE" build --target test -t "$IMAGE" -f tools/linux/Containerfile tools/linux
else
    echo "$IMAGE: built already" >&2
fi
ENVIRONMENT=""
for setting in "$@"; do
    ENVIRONMENT="$ENVIRONMENT --env $setting"
done
# --init: as the container's first process, xvfb-run never hears from Xvfb that its display is up
# shellcheck disable=SC2086 # each --env and the launch's words apart
"$ENGINE" run --rm --init -v "$PWD/build-linux":/build:ro -v "$DISC":/disc:ro -v "$OUT":/out -w /out \
    --env APPIMAGE_EXTRACT_AND_RUN=1 $HEADLESS $ENVIRONMENT "$IMAGE" \
    $LAUNCH /build/crash2-pc-linux.AppImage "/disc/$CUE"
