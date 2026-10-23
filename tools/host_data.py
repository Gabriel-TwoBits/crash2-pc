#!/usr/bin/env python3
"""Rewrites the decompilation's split data (splat's output from the player's executable) for the host's assembler: the
data, the bss and the read-only data, and the read-only blocks the game's C includes (INCLUDE_RODATA). Their numbers
become zeros, each run of them labelled, and retail_bytes.s lists the runs with their retail addresses: the program
copies the player's own executable's bytes there at start-up (src/retail_data.c), so it holds none of the game's data.
The symbols' addresses stay, for the linker. The output stays in the build directory, never in the repository.

    python tools/host_data.py DECOMP OUT [--coff]

DECOMP is the decompilation (the submodule), split by its configure.py; OUT receives macro.inc, data.s, retail_bytes.s,
and asm/'s included blocks at their paths, for the C's `.include` lines to find there. --coff writes them for 32-bit
Windows' COFF: its C symbols take a leading underscore, and its sections' flags are COFF's.
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
# A value of the split's data, its retail address in splat's comment: a number (an .asciz's bytes on the next line, its
# string's up to the first zero), or a symbol's or a jump table label's address, which starts with a letter, _ or .
DATUM = re.compile(r"^(\s*/\* [0-9A-F]+ ([0-9A-F]{8})(?: [0-9A-F]+)? \*/ )\.(byte|short|long|asciz) (.+)$")
STRING_BYTES = re.compile(r"^\s*/\* ([0-9A-F]+) \*/$")
DATUM_BYTES = {"byte": 1, "short": 2, "long": 4}
SYMBOLIC = re.compile(r"[A-Za-z_.]")
# A run stops at an alignment, so its words' offsets from its label are the retail ones whatever the padding
ALIGNMENT_LINE = re.compile(r"^\s*\.p2align\b")
# The table's sections and its entries: the run's label, its retail address and its bytes
TABLE_SECTION = ".section .rodata"
# Its words 4-aligned
TABLE_ALIGNMENT = 2
COFF_TABLE_SECTION = '.section .rdata, "dr"'
TABLES = {"writable": "g_RetailWritable", "read-only": "g_RetailReadOnly"}
RUN_LABELS = {"writable": "retail_writable_{}", "read-only": "retail_read_only_{}"}
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


class Runs:
    """The runs of numbers left out, labelled in the order they're found: (label, retail address, bytes)"""

    def __init__(self, label):
        self.label = label
        self.found = []
        self.open = None

    def add(self, address, size, value_is_zero):
        """A number's place; the label to put before it when it starts a run, else None. A zero extends a run but
        starts none: zeros are what the build leaves"""
        if self.open is not None and self.open[1] + self.open[2] == address:
            self.open[2] += size
            return None
        elif value_is_zero:
            self.close()
            return None
        else:
            self.close()
            self.open = [self.label.format(len(self.found)), address, size]
            self.found.append(self.open)
            return self.open[0]

    def close(self):
        self.open = None


def string_bytes(line):
    """An .asciz's bytes from splat's comment after it: its string's, its zero included"""
    found = STRING_BYTES.match(line or "")
    if found is None:
        sys.exit(f"an .asciz without its bytes after it: {line!r}")
    else:
        data = bytes.fromhex(found.group(1))
    if 0 not in data:
        sys.exit(f"an .asciz's bytes without its zero: {line!r}")
    else:
        return data, data.index(0) + 1


def blank(text, runs, coff):
    """The text with its numbers as zeros, each run of them after a global label runs records"""
    lines = text.split("\n")
    out = []
    prefix = "_" if coff else ""
    for index, line in enumerate(lines):
        datum = DATUM.match(line)
        if datum is None:
            if ALIGNMENT_LINE.match(line):
                runs.close()
            else:
                # A label, a comment or a section: the run goes on
                pass
            out.append(line)
            continue
        else:
            comment, address, kind, value = datum.group(1), int(datum.group(2), 16), datum.group(3), datum.group(4)
        if kind == "asciz":
            data, size = string_bytes(lines[index + 1] if index + 1 < len(lines) else None)
            label = runs.add(address, size, not any(data[:size]))
            blanked = f"{comment}.zero {size}"
        elif SYMBOLIC.match(value):
            # An address the linker writes
            runs.close()
            out.append(line)
            continue
        else:
            label = runs.add(address, DATUM_BYTES[kind], int(value, 0) == 0)
            blanked = f"{comment}.{kind} 0"
        if label is not None:
            out.append(f".global {prefix}{label}\n{prefix}{label}:")
        else:
            # Within a run, or a zero
            pass
        out.append(blanked)
    runs.close()
    return "\n".join(out)


def table(writable, read_only, coff):
    """retail_bytes.s: each run's label, retail address and bytes, the writable data's and the read-only data's"""
    prefix = "_" if coff else ""
    lines = ["/* The game's data the build leaves as zeros: where each run is, its retail address and its bytes, which "
             "src/retail_data.c copies from the player's executable */",
             COFF_TABLE_SECTION if coff else TABLE_SECTION, f".p2align {TABLE_ALIGNMENT}"]
    for name, runs in ((TABLES["writable"], writable), (TABLES["read-only"], read_only)):
        lines += [f".global {prefix}{name}", f"{prefix}{name}:"]
        lines += [f"    .long {prefix}{label}, 0x{address:08X}, {size}" for label, address, size in runs.found]
        lines += [f".global {prefix}{name}Count", f"{prefix}{name}Count:", f"    .long {len(runs.found)}"]
    return "\n".join(lines) + "\n"


def host(text, alignment, coff, runs):
    """splat's data directives as the host's assembler reads them, each section aligned to 1 << alignment, for COFF
    when coff is set, their numbers left to runs"""
    text = text.replace(BSS_SECTION, BSS_AS_DATA)
    text = WORD.sub(".long", text)
    text = ALIGN.sub(".p2align", text)
    text = blank(text, runs, coff)
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
    writable, read_only = Runs(RUN_LABELS["writable"]), Runs(RUN_LABELS["read-only"])
    for name, path in DATA_FILES.items():
        runs = read_only if name == "rodata.s" else writable
        (out / name).write_text(host((decomp / path).read_text(), alignments[name], coff, runs))
    blocks = 0
    for source in sorted((decomp / "src").rglob("*.c")):
        for folder, symbol in INCLUDE_RODATA.findall(source.read_text()):
            block = Path(folder) / f"{symbol}.s"
            if not (decomp / block).exists():
                sys.exit(f"{decomp / block} missing: split the decompilation again")
            else:
                (out / block).parent.mkdir(parents=True, exist_ok=True)
                (out / block).write_text(host((decomp / block).read_text(), BLOCK_ALIGNMENT, coff, read_only))
                blocks += 1
    (out / "retail_bytes.s").write_text(table(writable, read_only, coff))
    print(f"{out}: {len(DATA_FILES)} data files, {blocks} read-only blocks, "
          f"{len(writable.found) + len(read_only.found)} runs of the retail executable's bytes left out")


if __name__ == "__main__":
    main()
