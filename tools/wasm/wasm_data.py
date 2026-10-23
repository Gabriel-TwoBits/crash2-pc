"""The 64-bit build: the split's data as LLVM IR for wasm32, each value at its retail offset in one blob per file, each
label an alias into it. Numbers are zeros (src/retail_data.c copies them from the player's executable at start-up, as
the native build does); a word naming a symbol holds its address; a jump table's label 0; a retail address (80039D2C.c's
RETAIL_ADDRESS) its number.

    python wasm_data.py DECOMP OUT_DIR LL_FILE...

The LL_FILEs (the game's and the host's IR) give the functions' signatures, which a wasm reference must match.
"""
import re
import sys
from pathlib import Path

DATUM = re.compile(r"^\s*/\* (?:[0-9A-F]+ )?([0-9A-F]{8})(?: [0-9A-F]+)? \*/ "
                   r"\.(byte|short|half|word|asciz|space) (.+)$")
STRING_BYTES = re.compile(r"^\s*/\* ([0-9A-F]+) \*/$")
LABEL = re.compile(r"^dlabel (\w+)$")
INCLUDE_RODATA = re.compile(r'^INCLUDE_RODATA\("([^"]+)",\s*(\w+)\);$', re.MULTILINE)
RETAIL_ADDRESS = re.compile(r"RETAIL_ADDRESS\((\w+), (0x[0-9A-Fa-f]+)\)")
DEFINE = re.compile(r"^define [^@]*?(\S+) @(\w+)\(([^)]*)\)", re.MULTILINE)
WORD = 4
WORD_RANGE = 1 << 32
SIGN_LIMIT = 1 << 31
SIZES = {"byte": 1, "short": 2, "half": 2, "word": WORD}
HEX_BASE = 16
# Each blob's alignment in bytes
DATA_ALIGNMENT = 8
BSS_ALIGNMENT = WORD
RODATA_ALIGNMENT = 8
BLOCK_ALIGNMENT = WORD
# The split's sections, each one blob: (name, file, section, alignment, read-only), ahead of the blocks
SECTIONS = (
    ("retail_data", "asm/data/data.data.s", ".data.retail_data", DATA_ALIGNMENT, False),
    ("retail_bss", "asm/data/bss.bss.s", ".data.retail_bss", BSS_ALIGNMENT, False),
    ("retail_rodata", "asm/data/rodata.rodata.s", ".data.retail_rodata", RODATA_ALIGNMENT, True))


def i32(value):
    """An unsigned word as IR's i32 literal, which is signed"""
    return value - WORD_RANGE if value >= SIGN_LIMIT else value


def signatures(ll_files):
    found = {}
    for path in ll_files:
        for ret, name, params in DEFINE.findall(Path(path).read_text()):
            types = [re.sub(r" (noundef|signext|zeroext|nonnull|%\w+).*", "", p.strip()).split(" ")[0]
                     for p in params.split(",") if p.strip()]
            found[name] = (ret, types)
    return found


def parse(path):
    """The file's items: (address, size, kind, value) and its labels: (name, address)"""
    lines = Path(path).read_text().split("\n")
    items, labels, pending = [], [], []
    for index, line in enumerate(lines):
        label = LABEL.match(line)
        datum = DATUM.match(line)
        if label is not None:
            pending.append(label.group(1))
        elif datum is not None:
            address, kind, value = int(datum.group(1), HEX_BASE), datum.group(2), datum.group(3).strip()
            if kind == "asciz":
                hex_line = STRING_BYTES.match(lines[index + 1])
                data = bytes.fromhex(hex_line.group(1))
                size = data.index(0) + 1
            elif kind == "space":
                size = int(value, 0)
            else:
                size = SIZES[kind]
            for name in pending:
                labels.append((name, address))
            pending = []
            items.append((address, size, kind, value))
        else:
            # A directive, a comment or an alignment: the addresses place the items
            pass
    return items, labels


class Blob:
    """One global in a section, its fields in address order, its runs of numbers for the copy at start-up"""

    def __init__(self, name, section, items, labels, retail, functions, known):
        self.name, self.section = name, section
        self.start = items[0][0]
        self.fields, self.runs, self.labels = [], [], labels
        position = self.start
        for address, size, kind, value in items:
            if address > position:
                self.fields.append(f"[{address - position} x i8] zeroinitializer")
            elif address < position:
                sys.exit(f"{name}: 0x{address:08X} overlaps the item before")
            else:
                # Contiguous
                pass
            symbolic = kind == "word" and re.match(r"[A-Za-z_.]", value)
            if symbolic and value.startswith("."):
                self.fields.append("i32 0")
            elif symbolic and value in retail:
                self.fields.append(f"i32 {i32(int(retail[value], HEX_BASE))}")
            elif symbolic:
                self.fields.append(f"i32 ptrtoint (ptr @{value} to i32)")
                known.add(value)
            else:
                self.fields.append(f"[{size} x i8] zeroinitializer")
                if kind != "space" and not (kind != "asciz" and int(value, 0) == 0):
                    self.add_run(address, size)
                else:
                    # Zeros: what the build leaves
                    pass
            position = address + size
        self.end = position

    def add_run(self, address, size):
        if self.runs and self.runs[-1][0] + self.runs[-1][1] == address:
            self.runs[-1][1] += size
        else:
            self.runs.append([address, size])

    def ir(self, alignment):
        types = ", ".join(f.rsplit(" ", 1)[0] if f.endswith("zeroinitializer") else "i32" for f in self.fields)
        lines = [f"@{self.name} = hidden global <{{ {types} }}> <{{ {', '.join(self.fields)} }}>, "
                 f"section \"{self.section}\", align {alignment}"]
        for label, address in self.labels:
            offset = address - self.start
            lines.append(f"@{label} = hidden alias i8, getelementptr (i8, ptr @{self.name}, i32 {offset})")
        return lines

    def run_entries(self):
        return [f"{{ ptr, i32, i32 }} {{ ptr getelementptr (i8, ptr @{self.name}, i32 {address - self.start}), "
                f"i32 {i32(address)}, i32 {size} }}"
                for address, size in self.runs]


def main():
    decomp, out, ll_files = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:]
    out.mkdir(parents=True, exist_ok=True)
    functions = signatures(ll_files)
    # The C library's, which the split's data names too
    functions["printf"] = ("i32", ["ptr", "..."])
    retail = dict(RETAIL_ADDRESS.findall((decomp / "src/game/80039D2C.c").read_text()))
    known = set()
    blobs = []
    for name, path, section, align, read_only in SECTIONS:
        items, labels = parse(decomp / path)
        blobs.append((Blob(name, section, items, labels, retail, functions, known), align, read_only))
    for source in sorted((decomp / "src").rglob("*.c")):
        for folder, symbol in INCLUDE_RODATA.findall(source.read_text()):
            items, labels = parse(decomp / folder / f"{symbol}.s")
            blobs.append((Blob(f"block_{symbol}", f".data.block_{symbol}", items, labels, retail, functions, known),
                          BLOCK_ALIGNMENT, True))
    defined = {label for blob, _, _ in blobs for label, _ in blob.labels}
    head = ["; Generated by tools/wasm/wasm_data.py", 'target triple = "wasm32-unknown-wasip1"']

    def declarations(symbols, local):
        found = []
        for symbol in sorted(symbols - local):
            if symbol in functions:
                ret, params = functions[symbol]
                found.append(f"declare {ret} @{symbol}({', '.join(params)})")
            else:
                found.append(f"@{symbol} = external hidden global i8")
        return found

    def referenced(blob):
        return set(re.findall(r"ptr @(\w+)", "\n".join(blob.fields)))

    # The data, then (the game's files between) the bss, then the rest: the link keeps that order
    modules = {"retail_data_a.ll": [blobs[0]], "retail_data_c.ll": [blobs[1]], "retail_data_z.ll": blobs[2:]}
    for file_name, members in modules.items():
        lines = list(head)
        local = {label for blob, _, _ in members for label, _ in blob.labels} | {blob.name for blob, _, _ in members}
        refs = set()
        for blob, align, _ in members:
            lines += blob.ir(align)
            refs |= referenced(blob)
        if file_name == "retail_data_z.ll":
            for table, kind in (("g_RetailWritable", False), ("g_RetailReadOnly", True)):
                entries = [e for blob, _, read_only in blobs if read_only == kind for e in blob.run_entries()]
                row_type = "{ ptr, i32, i32 }"
                lines.append(f"@{table} = hidden constant [{len(entries)} x {row_type}] [{', '.join(entries)}]")
                lines.append(f"@{table}Count = hidden constant i32 {len(entries)}")
            refs |= {"retail_data"}
        else:
            # The tables are in the last module
            pass
        lines += declarations(refs, local)
        (out / file_name).write_text("\n".join(lines) + "\n")
    for blob, _, _ in blobs[:len(SECTIONS)]:
        print(f"{blob.name}: 0x{blob.start:08X} to 0x{blob.end:08X}, {len(blob.labels)} labels, {len(blob.runs)} runs")
    print(f"{len(blobs) - len(SECTIONS)} blocks; {len(known - defined)} symbols declared")


if __name__ == "__main__":
    main()
else:
    # Imported: nothing runs
    pass
