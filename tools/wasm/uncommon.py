"""The 64-bit build: a tentative definition (common) as a declaration: the split's bss defines it, as on ELF where they
merge"""
import re, sys
COMMON = re.compile(r"^(@\S+) = common ((?:[a-z_]+ )*)global (\S+(?: x \S+\])?) (?:zeroinitializer|0|null), "
                    r"align (\d+)$", re.M)
# The common definitions left that an error names
COMMON_SHOWN = 3
total = 0
for path in sys.argv[1:]:
    text = open(path).read()
    text, count = COMMON.subn(
        lambda m: f"{m.group(1)} = external {m.group(2)}global {m.group(3)}, align {m.group(4)}", text)
    total += count
    left = re.findall(r"^@\S+ = common .*$", text, re.M)
    if left:
        sys.exit(f"{path}: common definitions left: {left[:COMMON_SHOWN]}")
    else:
        # Each one converted
        pass
    open(path, "w").write(text)
print(total, "tentative definitions left to the split's bss")
