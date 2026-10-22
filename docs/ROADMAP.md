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
   - **GPU:** `src/gpu.c` holds the 1024×512 VRAM, reads the GP0 commands (psx-spx) and keeps the drawing state (E1
     to E6). SwanStation's software rasterizer draws (`third_party/swanstation`, GPL-3.0, from Mednafen's): polygons,
     lines, rectangles, fills, copies and loads with the GPU's own stepping, dithering, semi-transparency and mask.
     `src/gpu_raster.cpp` hands each primitive over as SwanStation's front end does. It replaced a rasterizer of the
     port's own (edge functions, attributes per pixel), for a picture the emulators' tests stand behind; headless, the
     run went from about 180 vertical blanks a second to about 310.
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
     took the program to 0x80796368, near the 8 MB `RAM_ADDRESS` tests for.
   - **The window's rest:** `cmake/game_data.ld` holds the rest of the 8 MB, to 0x80800000, as an empty section, which
     `main.c` makes unreadable. With libstdc++ linked, the C library's heap starts before `main`, right past the
     program, where a reservation at run time found it. Its Wayland window needs the 32-bit
     `libwayland-egl1` and `libwayland-cursor0` (README.md); without them it found no video device. X11 isn't built:
     its CMake finds no 32-bit `libX11.so`.
   - **Run:** the Naughty Dog logo over its nebula, Crash flying out at the title, the title screen with its menu as
     the PlayStation shows it (decomp's `tools/run_game.py`, `title_again`), then the attract demo's river level with
     its "DEMO" and timer, and back to the title. Headless with the rasterizer, about 180 vertical blanks a second; a
     change to the GPU's code that names its constants leaves the frame at blank 3,000 the same to the byte.
   - **Game clock:** `CRASH2_GAME_CLOCK=DIR` keeps decomp's `tools/run_game.lua` clock through decomp's
     `FRAME_ENDING()` on entering `Gfx_EndFrame`: the root counter's interrupts kept from the game, `g_TimerTicks` set
     to the game frame times 34. `DIR/game_frames.txt` takes its lines (game frame, vertical blank, a 64-bit FNV-1a
     over the displayed 16-bit pixels as 32-bit words): frame 0's, black, is the emulator's `3fd4ebc4ab9ce325`.
     `CRASH2_GAME_CLOCK_SHOTS` and `CRASH2_GAME_CLOCK_DUMPS` write a frame's picture and the game's data and bss at
     their retail addresses, as `run_game.py --dump`.
   - **Against the emulator, the pictures:** on the demo's 4,801 game frames the hashes agree only where the screen is
     black. The pictures can't agree to the pixel: PCSX-Redux draws with its own rasterizer. At game frame 2,800 both
     show the title's logo at the same point of its animation, 85.5% of the pixels the same; the nebula behind it and
     the stars sit elsewhere.
   - **Against the emulator, the state:** `tools/compare_ram.py` compares the game's variables (those its C names) in
     decomp's `run_game.py --dump` and the port's dumps, a word holding the same place on both sides apart: a pointer
     or a 24-bit link moved by the port's data, heap or scratchpad offset, or a pointer into code. On the demo:
     - game frame 3: 7 of 11,944 words differ, the sound's state (`D_8005FCF8`, the libraries' stubs) and
       `D_8006CCB8`, which `Sys_MainLoop` sets to what `Gfx_DrawWorlds` leaves in `$v0` and nothing reads;
     - game frame 50: 37 words, the sound's and the paging's: the PS1 has a page in flight (`D_80067FB0`,
       `D_80067FB4`) and its page slots at another stage, its CD taking time where the port's takes none;
     - game frame 1,000: 107 words, the same and what follows from them.
     So the port plays the PS1's game but for the sound and the CD's timing.
   - **Fidelity found by it:** `PutDrawEnv` builds its packet (E3, E4, E5, E1, E2, E6) in the structure's `dr_env`,
     where the PS1's RAM holds it, and sends it; the heap sits at the retail heap's place modulo 64 KB and the
     scratchpad at a 1 KB boundary, so the game's alignments of their addresses come out the same (`Gfx_OpenVideo`'s
     buffer padding, 4 bytes apart before).
   - **Left:** the CD's timing, for whole runs to stay together; speed.
7. **Input, sound, saves (in progress).**
   - **Input (done):** `src/input.c` reads the keyboard and the first gamepad through SDL3, and `src/psyq/pad.c`
     writes them into the pad's buffer at each vertical blank: a digital pad's buttons, 0 while pressed, in psx-spx's
     order. Without a window, no button is pressed.
   - **SPU (done):** SwanStation's (`third_party/swanstation/core/spu.cpp`, ADPCM voices, ADSR, reverb), driven by
     `src/spu_host.cpp` with each frame's CPU cycles, its 44.1 kHz frames to an SDL3 audio stream (`src/audio.c`).
     `src/psyq/libspu.c` writes its registers for the libspu functions the game calls, 10 of them by addresses the
     split leaves unnamed: SpuSetKey (0x80051078), SpuGetAllKeysStatus (0x8005182C), SpuSetVoiceAttr (0x800518B8),
     SpuGetVoiceAttr (0x800521A8, its volumes), SpuSetCommonAttr (0x800513D8), SpuSetTransferMode (0x800512F8),
     SpuSetTransferStartAddr (0x80051298), SpuWrite (0x80051238), SpuSetReverbModeParam (0x800501A8), SpuSetReverb
     (0x8004FE38), by their arguments and uses in decomp's `src/game/800324A8.c` and `800123F0.c`; the structures'
     layouts from how the game fills them. The reverb's presets are psx-spx's. The game's own sound effects go
     through them.
   - **Music (in progress):** `src/psyq/libsnd.c` plays the SEPs' sequences over the VABs from psx-spx's formats, ticked
     at each vertical blank (`SsSeqCalledTbyT`, which libsnd's data names), on the voices below `SsSetReservedVoice`'s
     count. SsSepStop (0x80056304), SsSepSetVol (0x80056760), SsSepSetCrescendo (0x800528C4) and SsSepSetDecrescendo
     (0x80052B4C) by their arguments and uses. Two facts of the formats psx-spx leaves out: the VAB's halfword at 0x12
     is its programs' count itself, not less one (the title's VAB has 13 programs, and its 30 VAG sizes, read after 13
     tone blocks, add up to its body's 0x3E6B0 bytes), and a SEQ's meta events carry no length (a tempo, FF 51, is
     followed by its 3 bytes). libsnd's formulas are measured: the SPU registers the PS1's libsnd writes in PCSX-Redux,
     caught with a write breakpoint, against the port's for the first 2,660 ticks of the intro's and the title's music.
     A side's volume is 0x3FFF times the square of the product of the velocity, the tone's and the VAB's volumes (each
     over 127) and the side's pan factor (the pan over 63, at most 1); the fine tuning counts 16ths of a semitone (the
     tone's byte shifted right 3). The 327 notes then start on the same samples at the same ticks, 323 at the same pitch
     and the PS1's other 4 one below, their volumes within 2 of the PS1's. In the second piece some notes start a tick
     later than the PS1's.
   - **Left:** the music against the PS1's note by note. `tools/compare_spu.py` on 12,000 vsyncs of the PS1 (intro,
     title, demos) against 8,000 of the port: of the notes aligned, 711 last another length, most cut on the PS1 when
     its libsnd takes their voice for a new note (its 16 voices all busy, it steals by a rule of its own); 69 start a
     blank apart, the tick's rounding alternating them; a tone's volume over 127 gives the PS1 a voice volume past
     0x3FFF, which the port's clamps. The programs', channels' and sequences' volumes and pans and the pitch bend's
     range aren't measured. Then the memory card in files.
   - **Next (decided 2026-10-22):** run the disc's own libsnd and libspu, their MIPS code from the player's disc as the
     game's data is, on `src/native.c`'s interpreter, so the music is the PS1's by construction: the interpreter
     extended to the instructions they use (lwl/lwr/swl/swr, mult/div, the rest of the loads and stores), libsnd's data
     reached at its retail addresses (the port's data keeps their layout, tools/check_layout.py), the SPU's registers
     and its DMA channel on the host's SPU, the OS calls they make (VSyncCallback, critical sections, root counters) on
     the host's, and the game's calls into them through the interpreter. `src/psyq/libsnd.c` then goes;
     `tools/compare_spu.py` checks it.
8. **Windows.** The same build for 32-bit Windows.
