#!/usr/bin/env python3
"""Rewrites the decompilation's split data (splat's output from the player's executable) for the host's assembler: the
data, the bss and the read-only data, and the read-only blocks the game's C includes (INCLUDE_RODATA). The output stays
in the build directory, never in the repository.

    python tools/host_data.py DECOMP OUT [--coff]

DECOMP is the decompilation (the submodule), split by its configure.py; OUT receives macro.inc, data.s, and asm/'s
included blocks at their paths, for the C's `.include` lines to find there. --coff writes them for 32-bit Windows'
COFF: its C symbols take a leading underscore, and its sections' flags are COFF's.
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
# A section's start, aligned as the retail one: the data's 8-aligned (0x8005B198), the bss 4-aligned (0x8005F2A4),
# where the small data before it ends; the split's offsets in it keep the retail alignments
SECTION = re.compile(r"^(\.section [^\n]*)$", re.MULTILINE)
SECTION_ALIGNMENTS = {"data.s": 3, "bss.s": 2, "rodata.s": 3}
# COFF's assembler pads a section's end to its alignment, which would leave the data's 4 bytes short of the small data
# after it: there the data's section is 4-aligned, its start 8-aligned by the linker script (cmake/game_data_pe.ld)
COFF_SECTION_ALIGNMENTS = {"data.s": 2, "bss.s": 2, "rodata.s": 3}
# The C's read-only blocks, which align their own words
BLOCK_ALIGNMENT = 2
# A word of a jump table into the split's own code (a label of its assembly): only that assembly reads it, which the
# host doesn't run
CODE_LABEL_WORD = re.compile(r"\.long \.L[0-9A-F]+\b")
# A word holding a symbol's address (not a number, not a jump table's local label)
SYMBOL_WORD = re.compile(r"(\.long )([A-Za-z_]\w*)\b")
# The sections' flags: ELF's allocated and writable, or allocated, as COFF's data and read-only data
COFF_SECTIONS = {'.section .data, "wa"': '.section .data, "dw"', '.section .rodata, "a"': '.section .rodata, "dr"'}
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
COFF_MACRO_INC = MACRO_INC.replace(".global \\label\n\\label:", ".global _\\label\n_\\label:")


# The bss as zeros in .data, where it follows the game's small data with nothing of the host's between
BSS_SECTION = '.section .bss, "wa"'
BSS_AS_DATA = '.section .data, "wa"'


def host(text, alignment, coff):
    """splat's data directives as the host's assembler reads them, each section aligned to 1 << alignment, for COFF
    when coff is set"""
    text = text.replace(BSS_SECTION, BSS_AS_DATA)
    text = WORD.sub(".long", text)
    text = ALIGN.sub(".p2align", text)
    # TRADEOFF: the split's jump tables hold 0 in place of their labels; a table the host's C reads would need them
    text = CODE_LABEL_WORD.sub(".long 0", text)
    if coff:
        for elf, coff_section in COFF_SECTIONS.items():
            text = text.replace(elf, coff_section)
        text = SYMBOL_WORD.sub("\\1_\\2", text)
    else:
        # ELF's names as they are
        pass
    return SECTION.sub(f"\\1\n.p2align {alignment}", text)


def main():
    arguments = [argument for argument in sys.argv[1:] if argument != "--coff"]
    coff = len(arguments) != len(sys.argv) - 1
    if len(arguments) != 2:
        sys.exit(__doc__)
    else:
        decomp, out = Path(arguments[0]), Path(arguments[1])
    missing = [str(decomp / path) for path in DATA_FILES.values() if not (decomp / path).exists()]
    if missing:
        sys.exit(f"{', '.join(missing)} missing: split the decompilation first (README.md, \"Building\")")
    else:
        out.mkdir(parents=True, exist_ok=True)
    (out / "macro.inc").write_text(COFF_MACRO_INC if coff else MACRO_INC)
    alignments = COFF_SECTION_ALIGNMENTS if coff else SECTION_ALIGNMENTS
    for name, path in DATA_FILES.items():
        (out / name).write_text(host((decomp / path).read_text(), alignments[name], coff))
    blocks = 0
    for source in sorted((decomp / "src").rglob("*.c")):
        for folder, symbol in INCLUDE_RODATA.findall(source.read_text()):
            block = Path(folder) / f"{symbol}.s"
            if not (decomp / block).exists():
                sys.exit(f"{decomp / block} missing: split the decompilation again")
            else:
                (out / block).parent.mkdir(parents=True, exist_ok=True)
                (out / block).write_text(host((decomp / block).read_text(), BLOCK_ALIGNMENT, coff))
                blocks += 1
    print(f"{out}: {len(DATA_FILES)} data files, {blocks} read-only blocks")


if __name__ == "__main__":
    main()
