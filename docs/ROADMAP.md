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
2. **The C made portable**, in the decompilation (its R7): the macros of decision 2, both of its executables byte for
   byte the same.
3. **A host build.** The decompilation's `src/game/` compiled for 32-bit x86 with the libraries' functions as stubs, its
   data generated from the player's executable, linked into one program that reaches the game's `main`.
4. **The game's loop on the host.** The CD read from the disc image, the heap, the timer and the vertical blank: the
   game boots and pages its first level in, headless.
5. **The picture.** The GTE's register moves on the host, the GPU's packets rasterized into VRAM, the display shown:
   the title screen, compared with the PlayStation build in an emulator.
6. **The native code.** The interpreter of the 32 instructions the GOOL entries' native blocks use.
7. **Input, sound, saves.** The pad from SDL3's gamepads and keyboard, the SPU and libsnd's sequencer, the memory card
   in files.
8. **Windows.** The same build for 32-bit Windows.
