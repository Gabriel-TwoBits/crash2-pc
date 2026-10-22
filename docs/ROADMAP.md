# Roadmap

## Decisions

Taken from the survey of what a host build of the C needs (crash2-decomp's docs/ROADMAP.md, section 6, "Beyond
matching"):

1. **A 32-bit build.** The game overwrites 32-bit slots of the disc's data with pointers (the NSF pages' and entries'
   offsets, the NSD's buckets), and the GOOL entries' native code reads objects at fixed offsets: a 32-bit build keeps
   every layout. A 64-bit one had 736 casts between pointers and integers of another size, and 50 layout checks
   failing. The cost: no macOS build, which has no 32-bit executables.
2. **The game's memory at the PlayStation's RAM, 0x80000000.** First taken as "anywhere, behind macros", revised
   when the host build ran. The game tells a pointer from an error by its sign: its errors are small negative numbers
   (`SUCCESS` is -0xFF, others -0x10 and -0x12), its pointers negative below them as KSEG0 addresses. So `Obj_Init`
   takes `Nsf_OpenEntry`'s entry for an error unless it is `< -0xFE`, as other callers do. A grep finds 31 such
   comparisons, and sign tests the C writes otherwise (the handwritten code's `bltz` on pointers) escape it.
   - At 0x08000000, `Obj_Init` left an object without its state, and `Obj_Update` read below its null stack.
   - At 0x80000000, the same build went on to the GOOL entries' native code.

   The program is linked there (`-Ttext-segment`), within the 8 MB `RAM_ADDRESS` tests for, and the decompilation's
   `RAM_BASE`, `RAM_ADDRESS` and `OT_LINK` hold as they are. The macros still give the host the scratchpad, the GTE's
   registers and the native code. The cost: a 32-bit Windows process needs `LARGEADDRESSAWARE` and its image based at
   0x80000000 (milestone 8).
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
   - **Memory:** the program, its data, the scratchpad and the heap sit at 0x80000000 (decision 2). `main.c` checks
     them at start-up (0x80000000 to 0x801EC76C), then hands the heap to `func_8001144C` as libsn's start-up does: the
     retail heap's 0x18FE08 bytes.
   - **Data layout:** the game's C reads across its variables' bounds, a word past one being its neighbour, so the
     link keeps the retail image's order and spacing. The split's data comes first, then each game file's small data
     in address order (`-fno-zero-initialized-in-bss -fno-toplevel-reorder -fdata-sections`, gathered 4-aligned by
     `cmake/game_data.ld` as GCC 2.7.2 aligned them), then the bss, kept in .data so nothing of the host's lands
     between. The host's code is position-independent, so the C library's `stderr` isn't copied among them.
     `tools/check_layout.py` checks after each link that all 936 of the game's data symbols sit at their retail
     addresses plus one offset. Before it, a write of the game's past `D_8005F17C`'s start set bit 18 of `stderr`'s
     copy, and glibc stopped the run.
   - **Libraries:** the 128 PsyQ functions the game and its data name are stubs that return 0 and log their first call
     (`src/psyq/stubs.c`). The BIOS's file calls, named like the C library's, are wrapped at the link
     (`src/psyq/bios_files.c`).
   - **Run:** `main` runs `Sys_Init` through: video, GTE, CD, GPU, pad, memory card, root counter 2's event. It then
     loops in `Cd_ReadFile`, retrying the read the stub doesn't make.
4. **The game's loop on the host (done).** `build/crash2 DISC.cue` (or `CRASH2_DISC`) boots headless: it pages its
   first level in and runs the main loop's objects, up to the first GOOL native block.
   - **CD:** `src/psyq/libcd.c` reads the disc image by sector: a .cue's .bin (MODE2/2352, 24 bytes to a sector's
     data) or a .iso. `CdlSetloc` and `CdlSeekL` move the position, and a read completes at once.
   - **Time:** `src/psyq/events.c` makes a VSync one vertical blank of the game's own time. Root counter 2 interrupts
     every 0x1000 ticks of the CPU's clock / 8, the mode 0x258 that libapi's `SetRCnt` writes it. In the proportion of
     an NTSC frame's CPU cycles (263 lines of 3,413 video cycles at 53.693175 MHz, against 33.8688 MHz), that is
     17.28 calls of `Sys_TimerHandler` a frame.
   - **SPU:** the paging waits on `func_80051328(0)` before and after a sound page's upload, which `src/psyq/libspu.c`
     answers as done.
   - **Pad:** `src/psyq/pad.c` fills `InitPAD`'s buffers at each vertical blank, as the BIOS does: a digital pad with no
     button pressed in port 1, none in port 2. Left zeroed, they read as a pad with every button held, and the game
     stayed on its first screen (level 60).
5. **The native code (done).** `src/native.c` interprets the 32 instructions the GOOL entries' native blocks
   use, on the registers decomp's `gool_native` sets. As on the R3000, a branch runs its delay slot, a load's register
   takes its value one instruction late, and a division by 0 leaves the CPU's results.
   - **Null pointers:** the PlayStation reads and writes its first 64 KB, the kernel's RAM, where Linux maps no page.
     The game goes there through null pointers: `Gool_ResolveRef`'s null operand (0xBE0), which `Gool_OpVector`
     reads. `src/low_ram.c` catches the fault, moves the instruction's address register onto a zeroed 64 KB copy, runs
     that one instruction under the trap flag, and moves the register back. Four sites so far.
   - **Run:** headless, with no button pressed, the game runs its title and attract demo for 30 seconds, about
     122,000 vertical blanks. Read every second, `NsfLevel.level` goes 30, 60, 14, 18, 35, 29, 9, then 30, 33, 14,
     18, 60, 29: the demo's levels and back to the title.
   - **Argument passing:** the decompilation's C passed two calls' arguments in registers the call didn't name, which
     the x86 doesn't keep. Fixed there (`PASSED_ON`, decomp/docs/ROADMAP.md section 6, "The macros").
   - **Debugging:** `CRASH2_SAMPLE` samples where the program spends its time (`src/sampler.c`); a fatal error
     aborts, its core dump read back by gdb. A live gdb can't follow the game past the low RAM's single steps.
   - **Test:** with `CRASH2_NATIVE_TRACE=FILE`, the interpreter writes each block's runs: its registers on entry,
     fetches, loads and stores with their values, and its registers at the end, each block's first 16 runs.
     `tools/check_native.py` replays them on Unicorn's MIPS (`tools/requirements.txt`), from the values the run found
     in memory, and compares the registers and the stored memory. On the title and attract demo (60 seconds), all
     10,041 runs agree: 669 of the 1,406 distinct blocks, by 900 paths. With `slt` made unsigned on purpose, 33 of 5,544
     differ.
   - **Left:** the blocks no demo level runs, with the levels played.
6. **The picture (in progress).** The display in an SDL3 window, from a GPU in software.
   - **GPU:** `src/gpu.c` holds the 1024×512 VRAM and runs the GP0 commands, from psx-spx: polygons (flat, Gouraud,
     textured through 4- and 8-bit CLUTs or 15-bit texels, raw or modulated), lines and polylines, rectangles, fills,
     VRAM copies and loads, the draw mode, texture window, drawing area, offset and mask. Semi-transparency, dithering
     and the mask bit as the GPU does them.
   - **libgpu:** `src/psyq/libgpu.c` walks the ordering table (24-bit links to KSEG0, decision 2), and sets the draw
     and display environments from the game's structures: DISPENV's 0x14 bytes, DRAWENV's dtd at 0x16 and dfe at 0x17,
     as `Gfx_Init` writes them.
   - **libgte:** `src/psyq/libgte.c` writes the GTE's registers (rotation, translation, offset, distance, far colour,
     light colours) and computes with decomp's GTE model, `RotTransPers` by its RTPS. `InitGeom` sets the retail
     executable's values: ZSF3 0x155, ZSF4 0x100, H 1000, DQA -0x1062, DQB 0x1400000. Left as stubs, the projection
     put every vertex at 0,0 and nothing showed.
   - **Display:** `src/video.c` shows PutDispEnv's rectangle of VRAM at 4:3, paced to the NTSC frame (263 lines of
     3,413 video cycles at 53.693175 MHz). `CRASH2_HEADLESS` runs without a window; `CRASH2_DUMP=DIR` writes the display
     every `CRASH2_DUMP_EVERY` blanks as PPM, all of VRAM with `CRASH2_DUMP_VRAM`.
   - **SDL3:** 3.2.10 from its release, pinned by SHA-256 and built as a shared library beside the program. Static, it
     took the program to 0x80796368, near the 8 MB `RAM_ADDRESS` tests for. Its Wayland window needs the 32-bit
     `libwayland-egl1` and `libwayland-cursor0` (README.md); without them it found no video device. X11 isn't built:
     its CMake finds no 32-bit `libX11.so`.
   - **Run:** the Naughty Dog logo over its nebula, Crash flying out at the title, the title screen with its menu as
     the PlayStation shows it (decomp's `tools/run_game.py`, `title_again`), then the attract demo's river level with
     its "DEMO" and timer, and back to the title. Headless with the rasterizer, about 180 vertical blanks a second; a
     change to the GPU's code that names its constants leaves the frame at blank 3,000 the same to the byte.
   - **Left:** a frame-by-frame comparison with the PlayStation build on the game clock, the levels' pictures, speed.
7. **Input, sound, saves.** The pad from SDL3's gamepads and keyboard, the SPU and libsnd's sequencer, the memory card
   in files.
8. **Windows.** The same build for 32-bit Windows.
