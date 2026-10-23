#!/bin/bash
# Spike: copies of the game's C and headers in wbuild/, which the IR passes compile, never the decompilation itself
cd "$(dirname "$0")/../.."
rm -rf wbuild/game-src && mkdir -p wbuild/game-src && cp decomp/src/game/*.c wbuild/game-src/
rm -rf wbuild/game-include && cp -r decomp/include wbuild/game-include
