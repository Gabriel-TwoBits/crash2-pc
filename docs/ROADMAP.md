# Roadmap

## Decisions

Taken from the survey of what a host build of the C needs (crash2-decomp's docs/ROADMAP.md, section 6, "Beyond
matching"):

1. **A 32-bit build.** The game overwrites 32-bit slots of the disc's data with pointers (the NSF pages' and entries'
   offsets, the NSD's buckets), and the GOOL entries' native code reads objects at fixed offsets: a 32-bit build keeps
   every layout. A 64-bit one had 736 casts between pointers and integers of another size, and 50 layout checks
   failing. The cost: no macOS build, which has no 32-bit executables.
2. **The game's memory anywhere, behind macros.** The points that assume the PlayStation's addresses (the scratchpad's
   base, the test for a pointer into RAM, the ordering table's 24-bit links, the GTE's register moves) become macros in
   the decompilation, the PlayStation's values by default, which the port redefines. Nothing is mapped at 0x80000000,
   which a 32-bit Windows process can't count on.
3. **SDL3, and a software renderer.** SDL3 for the window, input and audio. The GPU's packets drawn by a software
   rasterizer into a 1024×512 VRAM, the PlayStation's own model, before any GPU backend. The SPU in software, with
   libsnd's sequencer written for the host.

## Milestones

1. **The repository (done).** The decompilation as a submodule, this plan, the rules (CLAUDE.md).
2. **The C made portable (done)**, in the decompilation (its R7): `decomp/include/platform.h`'s macros, both of its
   executables byte for byte the same.
3. **A host build (done).** `CMakeLists.txt` compiles the decompilation's `src/game/` for 32-bit x86 (GCC 14.2,
   `-std=gnu89 -fcommon -fno-strict-aliasing -fwrapv`, `main` renamed `Game_Main`), with `src/host.h` included first.
   - **Data:** `tools/host_data.py` rewrites the split's data, bss and read-only data, and the C's 19 read-only blocks,
     for the host's assembler (`.word` as `.long`, `.align` as `.p2align`, each section 8-aligned). Their sizes are the
     retail ones: `.data` 0x3FE4 bytes from 0x8005B198, `.bss` 0xFF4C from 0x8005F2A4. The split's 86 words of jump
     tables into its own assembly hold 0.
   - **Memory:** the program, its data, the scratchpad and the heap sit in the 16 MB window at 0x08000000, where GNU
     ld puts a 32-bit program that isn't position-independent. So the ordering table's 24-bit links hold as the game
     writes them, and `RAM_ADDRESS` tests for that window. `main.c` checks the window at start-up (0x08048000 to
     0x082335D4), then hands the heap to `func_8001144C` as libsn's start-up does: the retail heap's 0x18FE08 bytes.
   - **Libraries:** the 128 PsyQ functions the game and its data name are stubs that return 0 and log their first call
     (`src/psyq/stubs.c`). The BIOS's file calls, named like the C library's, are wrapped at the link
     (`src/psyq/bios_files.c`).
   - **Run:** `main` runs `Sys_Init` through: video, GTE, CD, GPU, pad, memory card, root counter 2's event. It then
     loops in `Cd_ReadFile`, retrying the read the stub doesn't make.
4. **The game's loop on the host.** The CD read from the disc image, the heap, the timer and the vertical blank: the
   game boots and pages its first level in, headless.
5. **The picture.** The GTE's register moves on the host, the GPU's packets rasterized into VRAM, the display shown:
   the title screen, compared with the PlayStation build in an emulator.
6. **The native code.** The interpreter of the 32 instructions the GOOL entries' native blocks use.
7. **Input, sound, saves.** The pad from SDL3's gamepads and keyboard, the SPU and libsnd's sequencer, the memory card
   in files.
8. **Windows.** The same build for 32-bit Windows.
