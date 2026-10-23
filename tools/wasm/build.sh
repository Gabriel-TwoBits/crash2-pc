#!/bin/bash
# Spike: the port as a 64-bit native program, its code compiled for wasm32 (32-bit pointers, as the game's C needs)
# and turned into C by wasm2c (docs/ROADMAP.md, "64-bit through wasm"). Headless, the software picture only.
#   WASM_TOOLS=DIR [SDL_INCLUDE=DIR] tools/wasm-spike/build.sh
# DIR holds wasi-sdk/ (wasi-sdk 34) and wabt/ (WABT 1.0.42); SDL_INCLUDE is SDL3's include folder, which the port's
# CMake fetches. The decompilation is split in decomp/. Everything is written to wbuild/, wbuild/crash2-native last.
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${WASM_TOOLS:?set WASM_TOOLS to the folder holding wasi-sdk/ and wabt/}"
SDL_INCLUDE="${SDL_INCLUDE:-build/_deps/sdl3-src/include}"
SPIKE=tools/wasm-spike
CLANG="$WASM_TOOLS/wasi-sdk/bin/clang"
TARGET=(--target=wasm32-wasip1 "--sysroot=$WASM_TOOLS/wasi-sdk/share/wasi-sysroot")
RUNTIME="$WASM_TOOLS/wabt/share/wabt/wasm2c"
# The game's memory from 0x80070000, above the retail data's addresses (mips.c tells them apart from the host's), its
# 8 MB window holding the data and the bss; the module's memory reaches past 0x80000000 as the game's pointers' sign needs
GLOBAL_BASE=2147942400
STACK_BYTES=8388608
INITIAL_MEMORY=2181038080
MAX_MEMORY=4294967296
# The game's C as the native build compiles it, but for clang: its warnings there errors here
GAME_FLAGS=(-O0 -std=gnu89 -w -fcommon -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks
    -fno-zero-initialized-in-bss -fdata-sections -Wno-error=incompatible-function-pointer-types
    -Wno-error=int-conversion -Wno-error=incompatible-pointer-types "-D__asm__(...)=" -DNON_MATCHING -DGTE_MODEL
    -Dmain=Game_Main -Iwbuild/host-asm -Iwbuild/game-include -Idecomp/src/game -include src/host.h)
HOST_FLAGS=(-D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -Isrc -Iwbuild/sdk -I"$SDL_INCLUDE")
HOST_SOURCES=(src/main.c src/disc.c src/preferences.c src/audio.c src/game_clock.c src/gool.c src/menus.c
    src/settings.c src/host_log.c src/input.c src/gpu.c src/precise.c src/between.c src/gte.c src/mips.c src/sdk.c
    src/retail_data.c src/native.c src/handwritten.c src/video.c src/psyq/events.c src/psyq/libc.c src/psyq/libcd.c
    src/psyq/libgpu.c src/psyq/pad.c src/psyq/stubs.c src/psyq/card.c)
CXX_SOURCES=(src/gpu_raster.cpp src/spu_host.cpp third_party/swanstation/core/gpu_sw_backend.cpp
    third_party/swanstation/core/spu.cpp)
LIBGTE_FLAGS=(-std=gnu89 -w -fcommon -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks -DGTE_MODEL
    "-D__asm__(...)=" -Iwbuild/host-asm -Idecomp/include -include src/host.h)

mkdir -p wbuild/obj wbuild/hobj wbuild/hll wbuild/data wbuild/native
python3 -I tools/host_data.py decomp wbuild/host-asm > /dev/null
python3 -I tools/sdk_symbols.py decomp wbuild/sdk/sdk_symbols.h > /dev/null

echo "host: objects, and IR for the functions' signatures"
for source in "${HOST_SOURCES[@]}"; do
    name=$(echo "$source" | tr / _)
    "$CLANG" "${TARGET[@]}" -O2 -std=gnu11 "${HOST_FLAGS[@]}" -c "$source" -o "wbuild/hobj/${name%.c}.o"
    "$CLANG" "${TARGET[@]}" -O0 -std=gnu11 "${HOST_FLAGS[@]}" -S -emit-llvm "$source" -o "wbuild/hll/$name.ll"
done
"$CLANG" "${TARGET[@]}" -O2 "${HOST_FLAGS[@]}" "${LIBGTE_FLAGS[@]}" -c src/psyq/libgte.c -o wbuild/hobj/src_psyq_libgte.o
"$CLANG" "${TARGET[@]}" -O0 "${HOST_FLAGS[@]}" "${LIBGTE_FLAGS[@]}" -S -emit-llvm src/psyq/libgte.c -o wbuild/hll/src_psyq_libgte.c.ll
for source in "${CXX_SOURCES[@]}"; do
    name=$(echo "$source" | tr / _)
    "$CLANG++" "${TARGET[@]}" -O2 -std=c++17 -fno-exceptions -include cstdlib "${HOST_FLAGS[@]}" -Isrc/spu_shim \
        -Ithird_party/swanstation -Ithird_party/swanstation/core -c "$source" -o "wbuild/hobj/${name%.cpp}.o"
done
"$CLANG" "${TARGET[@]}" -O2 -std=gnu11 -Isrc -I"$SDL_INCLUDE" -c "$SPIKE/stubs.c" -o wbuild/stubs.o

echo "game: IR, adjusted for wasm, then objects"
"$SPIKE/prepare-game.sh"
rm -f wbuild/obj/*
for source in wbuild/game-src/*.c; do
    "$CLANG" "${TARGET[@]}" "${GAME_FLAGS[@]}" -S -emit-llvm "$source" \
        -o "wbuild/obj/$(basename "${source%.c}").ll"
done
python3 -I "$SPIKE/align4.py" wbuild/obj/*.ll
python3 -I "$SPIKE/uncommon.py" wbuild/obj/*.ll
# The BIOS's file calls, named as the C library's: card.c's, as the native link's --wrap gives them
sed -i -E 's/@(open|read|write|close)\b([(, ])/@__wrap_\1\2/g' wbuild/obj/*.ll
python3 -I "$SPIKE/fixcalls.py" "$(ls wbuild/obj/*.ll | paste -sd,)" "$(ls decomp/asm/data/*.s | paste -sd,)" \
    wbuild/obj/*.ll wbuild/hll/*.ll
for ir in wbuild/obj/*.ll; do
    "$CLANG" "${TARGET[@]}" -O0 -c "$ir" -o "${ir%.ll}.o" 2> /dev/null
done

echo "data: the split's, at their retail offsets"
python3 -I "$SPIKE/wasm_data.py" decomp wbuild/data wbuild/obj/*.ll wbuild/hll/*.ll
for ir in wbuild/data/*.ll; do
    "$CLANG" "${TARGET[@]}" -O0 -c "$ir" -o "${ir%.ll}.o" 2> /dev/null
done

echo "link: the data, the game's files, the bss, the rest, in that order"
"$CLANG++" "${TARGET[@]}" -o wbuild/crash2.wasm wbuild/data/retail_data_a.o $(ls wbuild/obj/*.o | sort) \
    wbuild/data/retail_data_c.o wbuild/data/retail_data_z.o wbuild/hobj/*.o wbuild/stubs.o \
    -Wl,--global-base=$GLOBAL_BASE -Wl,-z,stack-size=$STACK_BYTES -Wl,--initial-memory=$INITIAL_MEMORY \
    -Wl,--max-memory=$MAX_MEMORY -Wl,--no-gc-sections -Wl,--Map=wbuild/crash2.map \
    -lwasi-emulated-signal -lwasi-emulated-process-clocks

echo "native: wasm2c's C with the runner"
"$WASM_TOOLS/wabt/bin/wasm2c" wbuild/crash2.wasm -o wbuild/native/crash2.c -n crash2
for source in wbuild/native/crash2.c "$SPIKE/runner.c" "$RUNTIME/wasm-rt-impl.c" "$RUNTIME/wasm-rt-mem-impl.c" \
              "$RUNTIME/wasm-rt-exceptions-impl.c"; do
    gcc -O2 -w -I"$WASM_TOOLS/wabt/include" -I"$RUNTIME" -Iwbuild/native -c "$source" \
        -o "wbuild/native/$(basename "${source%.c}").o"
done
gcc -o wbuild/crash2-native wbuild/native/*.o -lm
echo "wbuild/crash2-native"
