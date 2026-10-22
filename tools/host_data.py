#!/usr/bin/env python3
"""Rewrites the decompilation's split data (splat's output from the player's executable) for the host's assembler: the
data, the bss and the read-only data, and the read-only blocks the game's C includes (INCLUDE_RODATA). The output stays
in the build directory, never in the repository.

    python tools/host_data.py DECOMP OUT

DECOMP is the decompilation (the submodule), split by its configure.py; OUT receives macro.inc, data.s, and asm/'s
included blocks at their paths, for the C's `.include` lines to find there.
"""
import re
import sys
from pathlib import Path

DATA_FILES = {
    "data.s": Path("asm") / "data" / "data.data.s",
    "bss.s": Path("asm") / "data" / "bss.bss.s",
    "rodata.s": Path("asm") / "data" / "rodata.rodata.s",
}
# The C's read-only blocks: INCLUDE_RODATA("asm/nonmatchings/...", NAME)
INCLUDE_RODATA = re.compile(r'^INCLUDE_RODATA\("([^"]+)",\s*(\w+)\);$', re.MULTILINE)
# MIPS gas's .word is 4 bytes and .align a power of two; on x86, .word is 2 bytes and .align counts bytes
WORD = re.compile(r"(?<=\s)\.word(?=\s)")
ALIGN = re.compile(r"(?<=\s)\.align(?=\s)")
# A section's start, aligned to 8 bytes: the retail sections start 4-aligned (.bss at 0x8005F2A4), so the split's
# offsets keep the retail alignments of up to 4 bytes
SECTION = re.compile(r"^(\.section [^\n]*)$", re.MULTILINE)
SECTION_ALIGNMENT = "\\1\n.p2align 3"
# A word of a jump table into the split's own code (a label of its assembly): only that assembly reads it, which the
# host doesn't run
CODE_LABEL_WORD = re.compile(r"\.long \.L[0-9A-F]+\b")
MACRO_INC = """\
.macro glabel label
    .global \\label
\\label:
.endm
.macro dlabel label
    .global \\label
\\label:
.endm
.macro jlabel label
\\label:
.endm
.macro enddlabel label
.endm
.macro endlabel label
.endm
.macro nonmatching label, size=1
.endm
"""


def host(text):
    """splat's data directives as the host's assembler reads them"""
    text = WORD.sub(".long", text)
    text = ALIGN.sub(".p2align", text)
    text = SECTION.sub(SECTION_ALIGNMENT, text)
    # TRADEOFF: the split's jump tables hold 0 in place of their labels; a table the host's C reads would need them
    return CODE_LABEL_WORD.sub(".long 0", text)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    decomp, out = Path(sys.argv[1]), Path(sys.argv[2])
    missing = [str(decomp / path) for path in DATA_FILES.values() if not (decomp / path).exists()]
    if missing:
        sys.exit(f"{', '.join(missing)} missing: split the decompilation first (README.md, \"Building\")")
    else:
        out.mkdir(parents=True, exist_ok=True)
    (out / "macro.inc").write_text(MACRO_INC)
    for name, path in DATA_FILES.items():
        (out / name).write_text(host((decomp / path).read_text()))
    blocks = 0
    for source in sorted((decomp / "src").rglob("*.c")):
        for folder, symbol in INCLUDE_RODATA.findall(source.read_text()):
            block = Path(folder) / f"{symbol}.s"
            if not (decomp / block).exists():
                sys.exit(f"{decomp / block} missing: split the decompilation again")
            else:
                (out / block).parent.mkdir(parents=True, exist_ok=True)
                (out / block).write_text(host((decomp / block).read_text()))
                blocks += 1
    print(f"{out}: {len(DATA_FILES)} data files, {blocks} read-only blocks")


if __name__ == "__main__":
    main()
