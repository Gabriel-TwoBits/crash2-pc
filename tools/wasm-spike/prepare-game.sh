#!/bin/bash
# Spike: copies of the game's C with what clang and wasm need, never the decompilation itself
cd "$(dirname "$0")/../.."
rm -rf wbuild/game-src && mkdir -p wbuild/game-src && cp decomp/src/game/*.c wbuild/game-src/
# Its retail return value, used before the declaration that drops it
sed -i 's/^extern void func_80051328(int);/extern int func_80051328(int);/' wbuild/game-src/800123F0.c
# Its retail code returns Ent_GetPropertyValue's value, which its callers read through a cast; the C drops it
python3 -I - <<'PY'
p = "wbuild/game-src/80023B04.c"
s = open(p).read()
old = "void func_80023D7C(void) {\n    Ent_GetPropertyValue("
assert s.count(old) == 1
open(p, "w").write(s.replace(old, "s32 func_80023D7C(void) {\n    return Ent_GetPropertyValue("))
PY
rm -rf wbuild/game-include && cp -r decomp/include wbuild/game-include
sed -i 's/^void func_80023D7C(void);/s32 func_80023D7C(void);/' wbuild/game-include/game.h
