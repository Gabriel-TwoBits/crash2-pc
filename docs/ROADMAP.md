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
   registers and the native code. The cost on Windows: the game in a library at 0x80000000, which a
   large-address-aware 32-bit process loads (milestone 8).
3. **SDL3, and a software renderer.** SDL3 for the window, input and audio. The GPU's packets drawn by a software
   rasterizer into a 1024×512 VRAM, the PlayStation's own model, before any GPU backend. The SPU in software, under
   the retail executable's own libsnd and libspu, run from the player's disc on the interpreter (2026-10-22, in place
   of a sequencer written for the host, which didn't reach the PS1's music: milestone 7).

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
   - **SPU:** the paging waits on `func_80051328(0)` (SpuIsTransferCompleted) before and after a sound page's upload,
     which the retail libspu answers (milestone 7).
   - **Pad:** `src/psyq/pad.c` fills `InitPAD`'s buffers at each vertical blank, as the BIOS does: a digital pad with no
     button pressed in port 1, none in port 2. Left zeroed, they read as a pad with every button held, and the game
     stayed on its first screen (level 60).
5. **The native code (done).** `src/native.c` runs the GOOL entries' native blocks, which use 32 instructions, on the
   registers decomp's `gool_native` sets, on the interpreter of `src/mips.c` (the R3000's integer instructions). As on
   the R3000, a branch runs its delay slot, a load's register takes its value one instruction late, and a division by 0
   leaves the CPU's results.
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
   - **Speed (done):** the build was unoptimized: SDL3's CMake leaves an unset build type at Debug. It now defaults to
     RelWithDebInfo (-O2, with symbols), the game's C kept at -O0: at -O2 it plays otherwise, the demo parting from the
     debug build's and stopping on a polyline of over 256 vertices, undefined behaviour somewhere in it still to find.
     Headless, the demo's 6,000 game frames went from 144 vertical blanks a second to 547, its game frames' hashes, SPU
     writes and the memory-card run's card the same to the byte as the debug build's, the native code agreeing with
     Unicorn.
   - **Left:** the CD's timing, for whole runs to stay together with the PS1's. A drive model alone won't do it: the PS1
     gives an unpaced game frame 1 to 6 vsyncs by its CPU's load, which the port doesn't model, so a read ends at
     another game frame whatever its length. It takes a paced run on both sides (run_game's pace, each game frame a set
     number of vsyncs, and the same pace on the port's clock) and the drive's timing as PCSX-Redux models it; a check
     for the tools, not for play, where a read ending at once only loads sooner. And the game's C at -O2.
7. **Input, sound, saves (done).**
   - **Input (done):** `src/input.c` reads the keyboard and the first gamepad through SDL3, and `src/psyq/pad.c`
     writes them into the pad's buffer at each vertical blank: a digital pad's buttons, 0 while pressed, in psx-spx's
     order. Without a window, no button is pressed.
   - **SPU (done):** SwanStation's (`third_party/swanstation/core/spu.cpp`, ADPCM voices, ADSR, reverb), driven by
     `src/spu_host.cpp` with each frame's CPU cycles, its 44.1 kHz frames to an SDL3 audio stream (`src/audio.c`).
   - **The sound libraries (done):** `src/sdk.c` runs the retail executable's libsnd and libspu, its code read from the
     disc's `SCUS_941.54`, on `src/mips.c`: the music and the sound effects are the PlayStation's own. The interpreter
     reads the executable's code and read-only data from the disc, its data and bss at the port's copy of them (their
     layout kept, `tools/check_layout.py`; a word where the port's copy holds one of its functions reads as the
     executable's), and the host's memory as it is. The game's calls into the libraries go through functions of
     `src/sdk.c` at their retail addresses (`tools/sdk_symbols.py`, from the split); the libraries' calls to the system
     come to hooks: critical sections, libetc's callbacks, libapi's events and root counters (`src/psyq/events.c`'s,
     which the game's own share), printf. Their hardware: the SPU's registers, the sound's DMA channel (a copy into the
     SPU's RAM at once), the interrupt controller, the memory control. What they need, as found: in the game's tick mode
     (`SsSetTickMode(1)`) libsnd ticks from the vertical blank's interrupt callback (`InterruptCallback(0,
     _SsTrapIntrVSync)`, root counter 3 at a target of 1); libspu's transfers end on an `EvSpTRAN` event its DMA
     callback delivers; its waits on the SPU's status need the SPU's time to move as they read it (64 cycles a read);
     and a key on needs it to move too: the game keys a sound effect's voice on and, later in the same frame, reads the
     voices' status (`SpuGetAllKeysStatus`), where an envelope still at 0 reads as the sound ended and the game frees
     the voice. With the SPU's time held, 213 of 490 sound effects in 8,000 frames ended in the frame they began (the
     PS1: 32 of 297, all the voices' resets); with a millisecond at each key on, 61, of which 56 are the resets and 5
     effects shorter than that.
   - **Against the PS1:** `tools/compare_spu.py` on 12,000 vsyncs of the PS1 (intro, title, demos) against 8,000 of the
     port: 1,763 notes aligned, each on the same sample, pitch, volumes and envelope as the PS1's, at the same blank
     within its run of aligned notes. 36 differ, at the 5 points where the game stops a piece or silences every voice:
     the PS1 runs in real time, the port on the game clock, which reach them at other times, and other demos after them,
     the notes on one side only. A libsnd written for the host from psx-spx's formats, with its formulas measured on the
     PS1, had 748 of 1,876 aligned notes differing (voices stolen by another rule, a tick's rounding): `src/sdk.c`
     replaced it.
   - **Memory card (done):** `src/psyq/card.c` keeps port 1's card in a 128 KB image (the emulators' .mcd), written
     through frame by frame, a missing one a formatted empty card. On it, the BIOS's file functions on "bu00:" (open,
     with create by blocks, read and write by 128-byte frames, close, erase, format, firstfile and nextfile) and
     libcard's calls, as psx-spx documents them: the directory in block 0, a file in 8 KB blocks chained from its first;
     the card new after power-on until its first write, `_new_card` passing the next frame's access; the events at once.
     A name of 20 characters fills DIRENTRY's name, the game reading on to the attribute's first byte, which stays 0
     (truncated to 19, the saved game couldn't be read back). Against the PS1, with run_game's memory-card schedule's
     presses on the game clock (`CRASH2_GAME_CLOCK_PRESSES`): both save one file of one block, and the two cards differ
     in 4 of 1,024 frames, none of them the format's: the file name's random digits and its entry's checksum, 15 bytes
     of the saved game's own state, which two PS1 runs save alike and the port's run, unpaced, saves otherwise, and the
     write-test frame, where the emulator's BIOS writes its buffer at `_card_clear`. Run again on that card, the port
     lists the save in Load Game.
8. **Windows (done under Wine).** The same build for 32-bit Windows, cross-compiled with MinGW-w64's GCC 14 in a
   container (`tools/windows/`), run under Wine 10 in it.
   - **The library at 0x80000000:** Wine reserves a 32-bit process's address space from 0x80000000 until the process
     runs, so a program based there fails to map (`STATUS_CONFLICTING_ADDRESSES`); a library loaded once it runs maps
     there. So the port is `crash2-game.dll`, based at 0x80000000 without ASLR, and `crash2.exe`, large-address-aware
     with Linux's 8 MB of stack, loads it from its folder and calls its `Host_Main`. Should 0x80000000 be taken, Windows
     moves the library and `main.c`'s check of the window stops it.
   - **PE's layout:** each section starts on a page, so the game's data section is page-aligned (8 bytes in ELF); the
     window's rest is a section to `ALIGN(0x800000)`, PE's ld keeping symbols relative to the image's base and refusing
     ELF's arithmetic on `.`; COFF's `-fdata-sections` names a variable's section `.data$NAME`; COFF's assembler pads a
     section's end to its alignment, so the split's data is 4-aligned there (8-aligned, it ended 4 bytes past the small
     data's start). `tools/check_layout.py` (with MinGW's nm, the names' underscore dropped) finds all 936 data symbols
     at their retail layout.
   - **Names:** 32-bit Windows' C symbols take a leading underscore: `tools/host_data.py --coff` writes the split's
     labels and words with it, and decomp's `ASM_SYMBOL` (`include/platform.h`, crash2-decomp 09646d0) gives
     `RETAIL_ADDRESS`'s symbols the host's prefix. The split's data had been taking decomp's MIPS ELF `macro.inc` on
     Linux too, its include path first: the build now puts tools/host_data.py's before it on both.
   - **The system's:** the low RAM's faults and single steps come to a vectored exception handler (Linux: signals), the
     window's rest is made unreadable by `VirtualProtect`, the heap is 64 KB-aligned at start-up (COFF aligns a variable
     to 8 KB at most), BSD's `bzero` and `bcopy` are the host's, the logs are written in binary, and a `.cue`'s path may
     use backslashes.
   - **Against the Linux build:** under Wine, headless, the demo's 6,000 game frames and the memory-card run's 3,500
     give the same frames' hashes, SPU writes and card, and the native code agrees with Unicorn; in a window on Xvfb,
     the first 600 game frames' hashes are the same. The zip (`tools/windows/build.sh`), unpacked, plays the same
     frames.
   - **Linux's AppImage:** built in a container too (`tools/linux/`), SDL3 with X11, Wayland, PulseAudio and ALSA. The
     program is 32-bit, which a 64-bit system rarely has libraries for, its C library included: the AppImage carries
     every library it loads (39, from 32-bit Debian trixie, 15 MB) and AppRun starts the program on the C library's own
     loader with them; SDL3 draws in software where it finds no OpenGL. In a 64-bit Debian with no 32-bit library, the
     demo's 6,000 game frames and the memory-card run give the same frames' hashes, SPU writes and card as the build's
     own runs, and in a window on Xvfb the first 600 game frames too.
   - **For players:** started without a disc image, the program opens the one chosen last (its path in `disc.txt` in
     SDL's preference folder) or asks for one in the system's file dialog (SDL3's: the desktop's portal on Linux, the
     common dialog on Windows); a dialog that can't open stops the program with the reason. A fatal error shows in a
     message box when a player can see it (not headless, not inside a fault's handler, where the box could wait on a
     lock the fault holds). `crash2.exe` is a window's program, with no console. In the test systems: the remembered
     image plays the same frames, a system with no portal and no zenity reports it, and under Wine the program waits on
     its dialog with no error.
   - **Left:** Windows itself, which no machine here runs, and the AppImage on a desktop: their windows, sound and
     gamepads through SDL3.
9. **Settings, in the game.** The game has no options screen: an overlay menu of the host's own, Dear ImGui (MIT)
   through SDL3, opened over the game by F1 or the gamepad's guide button, which pauses the game while open and leaves
   it where it was. Its choices are kept in `settings.ini` in SDL's preference folder: the window (fullscreen, size,
   vsync, the picture's aspect), the volume, the keyboard's and the gamepad's buttons; the enhancements below as they
   come. Closed and untouched, the game plays the same frames.
10. **A GPU renderer.** The GPU's packets drawn by SDL_GPU (Vulkan, Direct3D 12 or Metal) at a higher internal
    resolution (2×, 4×, the display's), with the PlayStation's VRAM as a texture: 24-bit colour without the dithering,
    optional texture filtering and anti-aliasing. The software renderer stays, the reference the frames' hashes and the
    comparisons with the PlayStation go by.
11. **Steady polygons.** The PlayStation's vertices snap to whole pixels (the jitter) and its textures are mapped
    without perspective (the warping). The GTE runs in the host's C: where it stores a screen coordinate
    (`GTE_STORE_DATA` of SXY, 61 places, nearly all in the world's and the objects' renderers) it can keep the
    coordinate's fraction and depth beside it, which the GPU renderer draws with, sub-pixel and perspective-correct. To
    find first: how much of the game copies coordinates past the macro.
12. **Widescreen.** A wider field of view through the GTE's projection, the game's culling of what falls outside a 4:3
    screen measured and widened where objects would vanish at the edges.
13. **The frame rate.** 30 frames a second as the PlayStation runs it, or more. Much of the game measures its time by
    `g_TimerTicks`, not all: what a game frame of 60 changes (physics, animations, the GOOL scripts) is measured first;
    failing that, the game's logic at 30 and the picture interpolated between its frames.
14. **Alongside.** The CD's timing; the game's C at -O2 (its undefined behaviour found); Windows itself.
