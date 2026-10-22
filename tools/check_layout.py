#!/usr/bin/env python3
"""Checks that the program holds the game's data as the retail image does: every symbol of the image's data, small data
and bss (0x8005B198 to 0x8006F1F0) at its retail address plus one offset. The game's C reads across its variables'
bounds (a word past one being its neighbour's), so the host keeps their order and spacing.

    python tools/check_layout.py PROGRAM SYMBOL_ADDRS [--nm NM] [--underscore]

PROGRAM is the linked program, SYMBOL_ADDRS the decompilation's config/symbol_addrs.txt; a D_XXXXXXXX symbol's address
is its name. --nm names the nm that reads the program (32-bit Windows': i686-w64-mingw32-nm); --underscore reads its C
symbols without the leading underscore 32-bit Windows gives them.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

DATA_START = 0x8005B198
DATA_END = 0x8006F1F0
NAMED = re.compile(r"^(\w+) = (0x[0-9A-Fa-f]+);")
ADDRESS_NAMED = re.compile(r"^D_([0-9A-F]{8})$")
# nm's lines: address, kind, name
NM_LINE = re.compile(r"^([0-9a-f]{8}) [bBdD] (\w+)$")
SHOWN = 20


def retail_addresses(symbol_addrs):
    found = {}
    for line in symbol_addrs.read_text().splitlines():
        named = NAMED.match(line)
        if named is not None:
            found[named.group(1)] = int(named.group(2), 16)
        else:
            continue
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("program", type=Path)
    parser.add_argument("symbol_addrs", type=Path)
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--underscore", action="store_true")
    arguments = parser.parse_args()
    program = arguments.program
    named = retail_addresses(arguments.symbol_addrs)
    output = subprocess.run([arguments.nm, str(program)], check=True, capture_output=True, text=True).stdout
    offsets = {}
    for line in output.splitlines():
        entry = NM_LINE.match(line)
        if entry is None:
            continue
        else:
            host, name = int(entry.group(1), 16), entry.group(2)
        if arguments.underscore and name.startswith("_"):
            name = name[1:]
        else:
            # The name as nm gives it
            pass
        by_name = ADDRESS_NAMED.match(name)
        if by_name is not None:
            retail = int(by_name.group(1), 16)
        elif name in named:
            retail = named[name]
        else:
            continue
        if DATA_START <= retail < DATA_END:
            offsets.setdefault(host - retail, []).append((retail, name))
        else:
            continue
    if not offsets:
        sys.exit(f"{program}: none of the game's data symbols")
    elif len(offsets) == 1:
        (offset, symbols), = offsets.items()
        print(f"{program}: the game's {len(symbols)} data symbols at their retail layout (offset {offset:#x})")
    else:
        main_offset = max(offsets, key=lambda key: len(offsets[key]))
        moved = sorted(symbol for key, symbols in offsets.items() if key != main_offset for symbol in symbols)
        lines = [f"  {name} retail {retail:#010x}" for retail, name in moved[:SHOWN]]
        sys.exit(f"{program}: {len(moved)} data symbols off the retail layout (offset {main_offset:#x} for the "
                 f"rest):\n" + "\n".join(lines))


if __name__ == "__main__":
    main()
