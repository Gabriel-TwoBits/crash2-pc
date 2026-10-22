#!/bin/sh
# Runs build-windows/crash2.exe under Wine in the Windows build's container, for the same checks as the Linux build's
# runs (CLAUDE.md, "Running and testing"):
#   tools/windows/run.sh [--window] DISC.cue OUT [NAME=VALUE ...]
# Headless, or with --window in a window on Xvfb's virtual display. The disc's folder is mounted read-only at Z:/disc,
# OUT at Z:/out; each NAME=VALUE goes to the program's environment, so a value naming a file takes its Z:/out path
# (CRASH2_GAME_CLOCK=Z:/out).
set -eu

if [ "${1:-}" = "--window" ]; then
    LAUNCH="xvfb-run -a wine"
    HEADLESS=""
    shift
else
    LAUNCH="wine"
    HEADLESS="--env CRASH2_HEADLESS=1"
fi
if [ $# -lt 2 ]; then
    sed -n '2,8p' "$0"
    exit 2
else
    DISC=$(cd "$(dirname "$1")" && pwd)
    CUE=$(basename "$1")
    OUT=$(mkdir -p "$2" && cd "$2" && pwd)
    shift 2
fi
IMAGE=crash2-pc-windows
ENGINE=$(command -v podman || command -v docker)
ENVIRONMENT=""
for setting in "$@"; do
    ENVIRONMENT="$ENVIRONMENT --env $setting"
done
# Wine's first start in a fresh prefix offers to install Mono and Gecko, a dialog that waits with a display: neither
# is needed. --init: as the container's first process, xvfb-run never hears from Xvfb that its display is up
# shellcheck disable=SC2086 # each --env and the launch's words apart
"$ENGINE" run --rm --init -v "$PWD":/project:ro -v "$DISC":/disc:ro -v "$OUT":/out -w /out \
    --env WINEDEBUG=-all --env "WINEDLLOVERRIDES=mscoree,mshtml=" $HEADLESS $ENVIRONMENT "$IMAGE" \
    $LAUNCH /project/build-windows/crash2.exe "Z:/disc/$CUE"
