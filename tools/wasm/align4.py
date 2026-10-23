"""Spike: each data variable a game file defines aligned to 4 at least, as the PlayStation's GCC 2.7.2 placed them, and
in a data section of its own even when zero (the wasm backend puts zeros in .bss): the link keeps them in order"""
import re, sys
# The alignment GCC 2.7.2 gave each data variable at least, in bytes
MIN_ALIGNMENT = 4
GLOBAL = re.compile(r'^(@(\S+) = (?:[a-z_]+ )*global .*?)(, section "[^"]*")?, align (\d+)$', re.M)
for path in sys.argv[1:]:
    text = open(path).read()
    def fix(m):
        name, section, align = m.group(2), m.group(3), int(m.group(4))
        if " external " in m.group(1) or " common " in m.group(1) or name.startswith(".") or name.startswith('"'):
            # A declaration, a tentative definition or a name of the compiler's: as it is
            return m.group(0)
        else:
            return f'{m.group(1)}{section or f", section \".data.{name}\""}, align {max(align, MIN_ALIGNMENT)}'
    text = GLOBAL.sub(fix, text)
    open(path, "w").write(text)
