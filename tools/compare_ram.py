#!/usr/bin/env python3
"""Compares the game's data and bss in two RAM dumps of the same game frame: decomp's tools/run_game.py --dump FRAME
(the PlayStation in an emulator, OUT/ram_FRAME.bin) against the port's CRASH2_GAME_CLOCK_DUMPS (DIR/ram_FRAME.bin, at
the same retail addresses). Only the variables the game's C names are compared: the libraries' own state is theirs,
which the port's doesn't keep. Each symbol with a differing word is listed with its words. A word that holds the same place
on both sides counts apart: the port's data, heap and scratchpad sit at other addresses, each moved by one offset
(src/main.c), and an address is a pointer or an ordering table's 24-bit link.

    python tools/compare_ram.py PS1.bin PORT.bin PROGRAM DECOMP

PROGRAM is the port's build/crash2, whose game data symbols tools/check_layout.py keeps at their retail layout; DECOMP
the decompilation, whose src/ names the game's variables.
"""
import bisect
import re
import struct
import subprocess
import sys
from pathlib import Path

DATA_START = 0x8005B198
BSS_END = 0x8006F1F0
RAM_BYTES = 0x200000
RAM_MASK = RAM_BYTES - 1
KSEG0 = 0x80000000
RAM_WINDOW_END = 0x80800000
RAM_END = KSEG0 + RAM_BYTES
LINK_MASK = 0xFFFFFF
WORD_MASK = 0xFFFFFFFF
SCRATCHPAD = 0x1F800000
SCRATCHPAD_BYTES = 0x400
# The heap's base on the PlayStation, and where src/main.c puts the port's: at the same place modulo 64 KB in its area
RETAIL_HEAP_BASE = 0x8006F1F0
HEAP_ALIGNMENT = 0x10000
HEAP_AREA = "s_HeapArea"
# The retail image's code, from its load address to its data; the port's ends at its first data symbol
RETAIL_CODE = 0x80010000
HOST_SCRATCHPAD = "g_HostScratchpad"
WORD = struct.Struct("<I")
ANCHOR = "g_EidAlphabet"
NM_LINE = re.compile(r"^([0-9a-f]{8}) [bBdD] (\w+)$")
SHOWN = 40
IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")


def symbols(program):
    """[(retail address, name)] of the program's data symbols, sorted, and the offsets from the PlayStation's
    addresses to the port's: the data's, the heap's, the scratchpad's"""
    output = subprocess.run(["nm", str(program)], check=True, capture_output=True, text=True).stdout
    found = {}
    for line in output.splitlines():
        entry = NM_LINE.match(line)
        if entry is not None:
            found[entry.group(2)] = int(entry.group(1), 16)
        else:
            continue
    missing = [name for name in (ANCHOR, HEAP_AREA, HOST_SCRATCHPAD) if name not in found]
    if missing:
        sys.exit(f"{program}: no {', '.join(missing)}")
    else:
        offset = found[ANCHOR] - DATA_START
    heap = found[HEAP_AREA] + (RETAIL_HEAP_BASE & (HEAP_ALIGNMENT - 1)) - RETAIL_HEAP_BASE
    scratchpad = found[HOST_SCRATCHPAD] - SCRATCHPAD
    table = sorted((host - offset, name) for name, host in found.items() if DATA_START <= host - offset < BSS_END)
    code_end = min(found.values())
    return table, (offset, heap), scratchpad, code_end


def game_names(decomp):
    """The identifiers the game's C uses"""
    found = set()
    for source in (decomp / "src").rglob("*.c"):
        found.update(IDENTIFIER.findall(source.read_text()))
    return found


def same_place(ps1, port, offsets, scratchpad, code_end):
    """Whether two words hold the same place: a pointer into the RAM or the scratchpad, or a 24-bit link, moved by one
    of the port's offsets; or both a pointer into code, which the port's compiler lays out otherwise"""
    if RETAIL_CODE <= ps1 < DATA_START and KSEG0 <= port < code_end:
        return True
    elif KSEG0 <= ps1 < RAM_END:
        return any((ps1 + offset) & WORD_MASK == port for offset in offsets)
    elif SCRATCHPAD <= ps1 < SCRATCHPAD + SCRATCHPAD_BYTES:
        return (ps1 + scratchpad) & WORD_MASK == port
    elif ps1 >> 24 == port >> 24 and (ps1 & LINK_MASK) < RAM_BYTES:
        return any((((ps1 & LINK_MASK) | KSEG0) + offset) & LINK_MASK == port & LINK_MASK for offset in offsets)
    else:
        return False


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    else:
        ps1, port, program, decomp = (Path(argument) for argument in sys.argv[1:])
    names = game_names(decomp)
    table, offsets, scratchpad, code_end = symbols(program)
    starts = [address for address, _ in table]
    left, right = ps1.read_bytes(), port.read_bytes()
    if len(left) < RAM_BYTES or len(right) < RAM_BYTES:
        sys.exit("a dump shorter than the RAM's 2 MB")
    else:
        compared = equal = places = 0
    differing = {}
    for address in range(DATA_START, BSS_END, WORD.size):
        index = bisect.bisect_right(starts, address) - 1
        if index < 0 or table[index][1] not in names:
            continue
        else:
            base, name = table[index]
        offset = address & RAM_MASK
        a, = WORD.unpack_from(left, offset)
        b, = WORD.unpack_from(right, offset)
        compared += 1
        if a == b:
            equal += 1
        elif same_place(a, b, offsets, scratchpad, code_end):
            places += 1
        else:
            differing.setdefault((base, name), []).append(f"+{address - base:#x}: {a:#x} {b:#x}")
    words = sum(len(lines) for lines in differing.values())
    print(f"{compared} words of the game's variables: {equal} equal, {places} the same place, {words} differ "
          f"in {len(differing)} symbols (PS1 first, then the port)")
    for (base, name), lines in sorted(differing.items())[:SHOWN]:
        print(f"  {name} ({base:#010x}): {len(lines)} words; " + ", ".join(lines[:4]))


if __name__ == "__main__":
    main()
