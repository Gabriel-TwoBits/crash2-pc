#!/usr/bin/env python3
"""Builds the 64-bit build's module: the game's C and the host's (all of it but the GPU renderer, src/render.c)
compiled for wasm32, whose 32-bit pointers the game's C needs, linked at the game's addresses and turned into C by
wasm2c, which the native program compiles (docs/ROADMAP.md, milestone 16).

    python tools/wasm/module.py --wasi-sdk DIR --wasm2c PATH --sdl-include DIR --out DIR --host SOURCE...

The game's C goes through LLVM IR, which the passes beside this script adjust for wasm: its variables 4-aligned in
sections of their own (align4.py), its tentative definitions left to the split's bss (uncommon.py), its calls given
their callees' signatures (fixcalls.py); the split's data is generated at its retail offsets (wasm_data.py). The
module imports SDL's functions and the GPU renderer's (the native program's, src/host64/bridge.c) and WASI's
(src/host64/wasi.c). OUT receives crash2_module.c and crash2_module.h.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
DECOMP = ROOT / "decomp"
# The game's memory from 0x80070000: above 0x80000000, as its pointers' sign needs, and above the retail data's
# addresses, which src/mips.c tells from the host's; its 8 MB window holds the data and the bss
GLOBAL_BASE = 0x80070000
STACK_BYTES = 0x800000
INITIAL_MEMORY = 0x82000000
MAX_MEMORY = 0x100000000
# The game's C as the native build compiles it (CMakeLists.txt), for clang: GCC's warnings among its errors. Its IR is
# clang's unoptimized one, which the passes below read, kept optimizable for its objects (GAME_OPTIMIZATION)
GAME_FLAGS = ["-O0", "-Xclang", "-disable-O0-optnone", "-std=gnu89", "-w", "-fcommon", "-fno-strict-aliasing",
              "-fwrapv", "-fno-delete-null-pointer-checks", "-fno-zero-initialized-in-bss", "-fdata-sections",
              "-Wno-error=incompatible-function-pointer-types", "-Wno-error=int-conversion",
              "-Wno-error=incompatible-pointer-types", "-D__asm__(...)=", "-DNON_MATCHING", "-DGTE_MODEL",
              "-Dmain=Game_Main"]
GAME_OPTIMIZATION = "-O2"
HOST_DEFINES = ["-D_WASI_EMULATED_SIGNAL", "-D_WASI_EMULATED_PROCESS_CLOCKS"]
# libgte's functions run the decompilation's GTE model, compiled with the game's C's semantics
LIBGTE = "src/psyq/libgte.c"
# The host's files that include the decompilation's headers (the native build's INCLUDE_DIRECTORIES, CMakeLists.txt),
# whose include_asm.h's top-level assembly wasm has no place for, as the game's C's
DECOMP_HEADERS = {"src/between.c"}
DECOMP_HEADER_FLAGS = [f"-I{DECOMP / 'include'}", "-D__asm__(...)="]
LIBGTE_FLAGS = ["-std=gnu89", "-w", "-fcommon", "-fno-strict-aliasing", "-fwrapv", "-fno-delete-null-pointer-checks",
                "-DGTE_MODEL", "-D__asm__(...)="]
# The functions the native program calls in the module
EXPORTS = ["Host_Setting", "Host_Vram", "Host_DisplayArea", "Host_ResolutionScale", "Precise_Find", "malloc", "free"]
# What the module may import: SDL's functions and the GPU renderer's, the native program's
IMPORTED = re.compile(r"^(SDL_\w+|Render_\w+)$")
IMPORT_DECLARATION = re.compile(r"w2c_env_(\w+)\(")
BIOS_FILE_CALLS = re.compile(r"@(open|read|write|close)\b([(, ])")
# The game's data as the retail image lays it out: the split's data, the game's files' variables, the split's bss
DATA_START = 0x8005B198
BSS_START = 0x8005F2A4
DATA_END = 0x8006F1F0
BLOBS = {"retail_data": DATA_START, "retail_bss": BSS_START}
MAP_SECTION = re.compile(r"^\s*([0-9a-f]{8})\s+\S+\s+\S+\s+\S+:\(\.data\.(\w+)\)")
SYMBOL_ADDRESS = re.compile(r"^(\w+) = (0x[0-9A-Fa-f]+);")
ADDRESS_NAMED = re.compile(r"^D_([0-9A-F]{8})$")
HEX_BASE = 16


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"{' '.join(str(c) for c in command)}\n{result.stdout}{result.stderr}")
    else:
        return result.stdout


def parallel(commands):
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for output in pool.map(run, commands):
            # Each command checked by run
            pass


def check_layout(map_file):
    """Every variable of the game's data at its retail address plus one offset, as the native build's
    tools/check_layout.py checks; the split's labels sit at their offsets in its blobs by construction"""
    retail = {}
    for line in (DECOMP / "config" / "symbol_addrs.txt").read_text().splitlines():
        found = SYMBOL_ADDRESS.match(line)
        if found is not None:
            retail[found.group(1)] = int(found.group(2), HEX_BASE)
        else:
            # A comment or another kind of line
            pass
    placed = {found.group(2): int(found.group(1), HEX_BASE)
              for found in map(MAP_SECTION.match, map_file.read_text().splitlines()) if found is not None}
    offset = placed["retail_data"] - DATA_START
    wrong, checked = [], 0
    for name, address in placed.items():
        named = ADDRESS_NAMED.match(name)
        if name in BLOBS:
            expected = BLOBS[name]
        elif name in retail:
            expected = retail[name]
        elif named is not None:
            expected = int(named.group(1), HEX_BASE)
        else:
            # The host's own data
            continue
        if not DATA_START <= expected < DATA_END:
            continue
        elif address - expected != offset:
            wrong.append(f"{name} at 0x{address:08X}, not 0x{expected + offset:08X}")
        else:
            checked += 1
    if wrong:
        sys.exit(f"the game's data out of its retail layout: {'; '.join(wrong)}")
    else:
        return checked, offset


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wasi-sdk", required=True, type=Path)
    parser.add_argument("--wasm2c", required=True, type=Path)
    parser.add_argument("--sdl-include", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--host", nargs="+", required=True, help="the host's C and C++ sources in the module")
    arguments = parser.parse_args()
    clang = arguments.wasi_sdk / "bin" / "clang"
    target = ["--target=wasm32-wasip1", f"--sysroot={arguments.wasi_sdk / 'share' / 'wasi-sysroot'}"]
    out = arguments.out
    for folder in ("game", "host", "data", "sdk"):
        (out / folder).mkdir(parents=True, exist_ok=True)
    run([sys.executable, str(ROOT / "tools" / "sdk_symbols.py"), str(DECOMP), str(out / "sdk" / "sdk_symbols.h")])
    host_flags = HOST_DEFINES + [f"-I{ROOT / 'src'}", f"-I{out / 'sdk'}", f"-I{arguments.sdl_include}"]
    game_flags = GAME_FLAGS + [f"-I{DECOMP / 'include'}", f"-I{DECOMP / 'src' / 'game'}", "-include",
                               str(ROOT / "src" / "host.h")]

    # The host: objects, and IR for the signatures of the functions the game's C calls
    commands, host_objects, host_ir = [], [], []
    for source in arguments.host:
        name = source.replace("/", "_")
        path = ROOT / source
        obj, ir = out / "host" / f"{name}.o", out / "host" / f"{name}.ll"
        if source.endswith(".cpp"):
            commands.append([str(clang) + "++", *target, "-O2", "-std=c++17", "-fno-exceptions", "-include", "cstdlib",
                             *host_flags, f"-I{ROOT / 'src' / 'spu_shim'}", f"-I{ROOT / 'third_party' / 'swanstation'}",
                             f"-I{ROOT / 'third_party' / 'swanstation' / 'core'}", "-c", str(path), "-o", str(obj)])
        elif source == LIBGTE:
            libgte = [*host_flags, *LIBGTE_FLAGS, f"-I{DECOMP / 'include'}", "-include", str(ROOT / "src" / "host.h")]
            commands.append([str(clang), *target, "-O2", *libgte, "-c", str(path), "-o", str(obj)])
            commands.append([str(clang), *target, "-O0", *libgte, "-S", "-emit-llvm", str(path), "-o", str(ir)])
            host_ir.append(ir)
        else:
            flags = host_flags + (DECOMP_HEADER_FLAGS if source in DECOMP_HEADERS else [])
            commands.append([str(clang), *target, "-O2", "-std=gnu11", *flags, "-c", str(path), "-o", str(obj)])
            commands.append([str(clang), *target, "-O0", "-std=gnu11", *flags, "-S", "-emit-llvm", str(path), "-o",
                             str(ir)])
            host_ir.append(ir)
        host_objects.append(obj)

    # The game's C as IR
    game_sources = sorted((DECOMP / "src" / "game").glob("*.c"))
    game_ir = [out / "game" / f"{source.stem}.ll" for source in game_sources]
    commands += [[str(clang), *target, *game_flags, "-S", "-emit-llvm", str(source), "-o", str(ir)]
                 for source, ir in zip(game_sources, game_ir)]
    parallel(commands)

    # Adjusted for wasm, then objects
    run([sys.executable, "-I", str(HERE / "align4.py"), *map(str, game_ir)])
    run([sys.executable, "-I", str(HERE / "uncommon.py"), *map(str, game_ir)])
    # The BIOS's file calls, named as the C library's: card.c's, as the native link's --wrap gives them
    for ir in game_ir:
        ir.write_text(BIOS_FILE_CALLS.sub(r"@__wrap_\1\2", ir.read_text()))
    split = ",".join(str(path) for path in sorted((DECOMP / "asm" / "data").glob("*.s")))
    print(run([sys.executable, "-I", str(HERE / "fixcalls.py"), ",".join(map(str, game_ir)), split,
               *map(str, game_ir), *map(str, host_ir)]).strip())
    print(run([sys.executable, "-I", str(HERE / "wasm_data.py"), str(DECOMP), str(out / "data"),
               *map(str, game_ir), *map(str, host_ir)]).strip())
    data_ir = [out / "data" / name for name in ("retail_data_a.ll", "retail_data_c.ll", "retail_data_z.ll")]
    parallel([[str(clang), *target, GAME_OPTIMIZATION, "-Wno-override-module", "-c", str(ir),
               "-o", str(ir.with_suffix(".o"))] for ir in game_ir]
             + [[str(clang), *target, "-O0", "-Wno-override-module", "-c", str(ir), "-o", str(ir.with_suffix(".o"))]
                for ir in data_ir])

    # Linked: the split's data, the game's files' variables, the split's bss, in that order, then the rest
    module = out / "crash2.wasm"
    run([str(clang) + "++", *target, "-o", str(module), str(data_ir[0].with_suffix(".o")),
         *[str(ir.with_suffix(".o")) for ir in game_ir], str(data_ir[1].with_suffix(".o")),
         str(data_ir[2].with_suffix(".o")), *map(str, host_objects),
         f"-Wl,--global-base={GLOBAL_BASE}", f"-Wl,-z,stack-size={STACK_BYTES}",
         f"-Wl,--initial-memory={INITIAL_MEMORY}", f"-Wl,--max-memory={MAX_MEMORY}", "-Wl,--no-gc-sections",
         "-Wl,--allow-undefined", "-Wl,--export-table", f"-Wl,--Map={out / 'crash2.map'}",
         *[f"-Wl,--export={name}" for name in EXPORTS], "-lwasi-emulated-signal", "-lwasi-emulated-process-clocks"])
    checked, offset = check_layout(out / "crash2.map")
    print(f"{module}: the game's data at its retail layout ({checked} variables and blobs, offset {offset:#x})")
    run([str(arguments.wasm2c), str(module), "-o", str(out / "crash2_module.c"), "-n", "crash2"])
    imports = set(IMPORT_DECLARATION.findall((out / "crash2_module.h").read_text()))
    unexpected = sorted(name for name in imports if not IMPORTED.match(name))
    if unexpected:
        sys.exit(f"the module imports what the native program doesn't give: {', '.join(unexpected)}")
    else:
        print(f"{out / 'crash2_module.c'}: {len(imports)} functions imported from the native program")


if __name__ == "__main__":
    main()
else:
    # Imported: its functions only
    pass
