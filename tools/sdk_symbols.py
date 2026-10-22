#!/usr/bin/env python3
"""Writes the retail addresses of the libraries' functions, from the decompilation's split (asm/sdk/, splat's output
from the player's executable), as a C header for src/sdk.c: `#define SDK_NAME 0xADDRESSu` for each function. The
output stays in the build directory, never in the repository.

    python tools/sdk_symbols.py DECOMP HEADER
"""
import re
import sys
from pathlib import Path

SDK = Path("asm") / "sdk"
FUNCTION = re.compile(r"^\s*glabel (\w+)\s*$")
# splat's instruction lines: /* OFFSET ADDRESS WORD */ mnemonic
INSTRUCTION = re.compile(r"^\s*/\* [0-9A-F]+ ([0-9A-F]{8}) [0-9A-F]{8} \*/")
HEXADECIMAL = 16


def addresses(decomp):
    """{name: address} of each function of the split's libraries: a glabel's first instruction's address"""
    found = {}
    for path in sorted((decomp / SDK).rglob("*.s")):
        pending = None
        for line in path.read_text().splitlines():
            function = FUNCTION.match(line)
            instruction = INSTRUCTION.match(line)
            if function is not None:
                pending = function.group(1)
            elif instruction is not None and pending is not None:
                address = int(instruction.group(1), HEXADECIMAL)
                if found.get(pending, address) != address:
                    sys.exit(f"{path}: {pending} at 0x{address:08X} and 0x{found[pending]:08X}")
                else:
                    found[pending] = address
                pending = None
            else:
                continue
    return found


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    else:
        decomp, header = Path(sys.argv[1]), Path(sys.argv[2])
    if not (decomp / SDK).is_dir():
        sys.exit(f"{decomp / SDK} missing: split the decompilation first (README.md, \"Building\")")
    else:
        found = addresses(decomp)
    lines = ["/* The libraries' functions at their retail addresses (tools/sdk_symbols.py) */",
             "#ifndef SDK_SYMBOLS_H", "#define SDK_SYMBOLS_H"]
    lines += [f"#define SDK_{name} 0x{address:08X}u" for name, address in sorted(found.items())]
    lines.append("#endif")
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text("".join(f"{line}\n" for line in lines))
    print(f"{header}: {len(found)} functions")


if __name__ == "__main__":
    main()
