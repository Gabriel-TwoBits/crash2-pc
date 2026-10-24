# Roadmap

## Decisions

Taken from the survey of what a host build of the C needs (crash2-decomp's docs/ROADMAP.md, section 6, "Beyond
matching"):

1. **A 32-bit build.** The game overwrites 32-bit slots of the disc's data with pointers (the NSF pages' and entries'
   offsets, the NSD's buckets), and the GOOL entries' native code reads objects at fixed offsets: a 32-bit build keeps
   every layout. A 64-bit one had 736 casts between pointers and integers of another size, and 50 layout checks
   failing. The cost was no macOS build, which has no 32-bit executables: a 64-bit build now goes through wasm32,
   whose pointers are 32 bits (milestone 16).
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
4. **None of the game's data in the program (2026-10-23).** The split's data, read-only data and the C's read-only
   blocks went into the program as splat wrote them, so a release carried the executable's tables and strings (the
   0.1.0 one's library held 83 of its 84 strings of 12 characters or more). `tools/host_data.py` now writes their
   numbers as zeros, each run of them at a label, and lists the runs with their retail addresses (`retail_bytes.s`, 127
   runs); `src/retail_data.c` copies them from the player's executable at start-up, before the game's first
   instruction, the read-only ones' pages made writable for it. The symbols' addresses stay the linker's. The program
   then shares one string of the executable's, the window's title; what else it holds of it is the decompilation's own
   C (its variables defined with their values). The demo's, the card's and a player's run's frames, sound and card,
   and the GPU renderer's pictures, are the same to the byte, on Linux and under Wine.

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
6. **The picture (done).** The display in an SDL3 window, from a GPU in software. What it left, the CD's timing and the
   game's C at -O2, concerns the tools' comparisons with the PS1 and the speed, not play: milestone 14 carries both.
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
     debug build's and stopping on a polyline of over 256 vertices, undefined behaviour somewhere in it (found in
     milestone 14, and the game's C optimized since).
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
9. **Settings, in the game (done).** The port's options sit in the game's own menus, reached with the pad: OPTIONS, a
   third item on the title's menu, and PC OPTIONS in the pause menu's OPTIONS, before DONE. Either shows the settings in
   the menu's place, in its font and look (each row its name and value between the font's arrows, "<FPS: 60>", as the
   pause options' "<CTR: 0>"), and DONE, triangle or circle go back. The settings' rows scroll under DONE, which stays
   in view (4 rows on the title, 5 in the pause menu, across all of its panel: the game leaves the paused picture above
   it as it is, a text drawn there staying after it). The menu's own GOOL code moves the cursor, with its sounds;
   `src/menus.c` patches four words of each menu's code where the game loads it (TitOC's, and DispC's as the warp rooms
   and levels load it; the title's DispC is another entry), each an operation of the host's (decomp's
   `GOOL_HOST_OPERATION`) doing the replaced instruction's work besides its own, and only where each word holds that
   instruction. TitOC's code already handles a third item, the PAL release's language, unreachable here: its sides are
   left out. The settings, in `settings.ini` in the preference folder (headless, the defaults, never saved): the window
   or full screen, the picture at 4:3 or stretched, sharp or smooth; the enhancements below add theirs. The game plays
   the same frames but for the title menu's picture: the demo's run and the card's, their sound and the card, as before.
10. **A GPU renderer (done).** `src/render.c` draws each primitive gpu.c hands the software rasterizer again, with
    SDL_GPU on Vulkan (SPIR-V from `shaders/`, compiled by glslang at the build), into a target of VRAM's size times
    the resolution (2, 3, 4, 6 or 8), in true colour without the dithering; the window shows the target's displayed
    rectangle, or at the PlayStation's resolution the software picture. The software rasterizer keeps drawing: its VRAM
    stays the game's and the textures' source, so the game plays the same frames whatever the renderer does.
    - **Textures:** `draw.frag` reads them from the software VRAM as the GPU does (4- and 8-bit CLUTs, 15-bit), the
      texture window applied and each coordinate held within its primitive's (no seams at the higher resolution). The
      VRAM is uploaded again after a load, copy or fill, before the next textured draw. TRADEOFF: a draw reading what
      the frame drew before (the framebuffer as a texture) reads it as of that upload.
    - **Blending:** the four semi-transparencies as blend states, a texture's texels without their bit drawn opaque
      in a pass of their own. The primitives are moved half a target pixel so the target samples at the corners of the
      PlayStation's pixels, as its GPU does.
    - **VRAM:** a CPU's load is drawn into the target from the software VRAM, a copy copies the target, a fill fills.
    - **Checked:** the title, the attract demo's levels, the warp room's pause screen at 4× on lavapipe and on RADV
      (the RX 9060 XT: the demo at some 500 game frames a second headless, the software rasterizer included), the
      demo's and the card run's frames, sound and card the same as without it; the AppImage's window on lavapipe; Wine,
      with no Vulkan, falls back to the software picture. Windows itself takes Vulkan from the card's driver; D3D12
      (DXIL shaders) would serve where none is.
    - **Found on the way:** with no window nothing waited on the GPU, and SDL_GPU's copies of the buffers ran the
      32-bit address space out: two pictures at most are in flight. `low_ram.c` helped any thread's fault near 0, the
      driver's too: it now helps only the port's own code.
    - **Left:** texture filtering, anti-aliasing.
11. **Steady polygons (done).** The GTE rounds a projected vertex to whole pixels, so the PlayStation's polygons jitter,
    and its GPU steps a texture without the depth, so they warp. decomp's GTE model hands over each projection's view
    coordinates before their rounding (`GTE_PROJECTED`, crash2-decomp 152ade9); `src/precise.c` computes the position
    again with their fractions and keeps it with its depth, by the value of the SXY the GTE wrote. The game copies that
    value into its primitives, directly or through its scratchpad (the 61 stores of the SXY registers do both), so the
    GPU renderer finds each vertex's projection by its value, this game frame's or the last's. A polygon is moved by
    its vertices' fractions (STEADY) and its texture and colours stepped with their depths (UNWARPED) only when each of
    its vertices has one: a polygon the GTE didn't project (the font's, the HUD's) stays as written. On the demo some
    99.8% of the polygons drawn have all theirs. Vertices projected to the same pixel in a frame (4% of those drawn on
    Turtle Woods, Crash's before the walls behind him) keep a projection each; a polygon's vertex takes the one whose
    depth is nearest its other vertices'. Taking the later alone, 2.2% of the vertices drawn took another's, and their
    polygons' textures flickered as Crash moved. TRADEOFF: following each value through the game's memory (PGXP) would
    tell them apart for sure. Both are options, off as the PlayStation draws, shown with the GPU renderer.
    - **Kept within the GTE's limits:** a projection was first kept only when its position came within 1.5 pixels of the
      GTE's, but the GTE floors IR1-2 before it multiplies (up to H / SZ3 pixels more) and its division errs more far
      from the screen's centre, so near vertices (H 288 and z some 400 on Snow Go's path, 1.7 pixels) and those off the
      screen were dropped, their polygons drawn as written beside steady neighbours: on the recorded Snow Go run at 8×,
      63,481 of 3.2 million polygons drawn had some vertices and not others, switching between the two as their vertices
      crossed the tolerance, and the pause screen's crystal and gem had black wedges and broken facets. A projection is
      now kept unless the GTE saturated IR1-2, SZ3 or the position or its division overflowed: 4,959 left (the positions
      past the screen's limits among them), the models as the software picture draws them. The slots' probes full (180
      times on that run), the frame already drawn gives up its projection rather than the frame being built.
12. **Widescreen (done).** The shape's third value, 16:9: decomp's GTE model lets a host narrow each projection's
    horizontal offset from the screen's centre (`GTE_HORIZONTAL`, crash2-decomp f7eb8a3), which the port does by 3/4,
    and the picture is shown at 16:9, so a 4:3 picture's worth of the world takes three quarters of the width and the
    rest shows what lay beside it. The HUD and the menus, projected too, keep their shape at the middle. The world's
    geometry is there at the edges on the attract demo's levels (the game culls it on the screen's coordinates, after
    the projection); objects the game culls by its own 4:3 bounds would come in late at the edges, none seen so far.
    The game's state follows the narrower projection (a different picture, the same play otherwise); the default 4:3
    plays the same frames as before. The software picture narrows too.
    - **Zoomed:** the shape's fourth value, ZOOM, shows the 4:3 picture's middle three quarters of its height at
      16:9, its top and bottom cut: no wider view, no edges.
    - **The world beside the view, tried and dropped:** the game draws only the polygons its camera path's sort list
      (SLST) holds at the camera's node, made for 4:3, so at 16:9 the world comes and goes at the edges. A host's list
      (`WORLD_SORT_LIST`, with larger frame buffers through `FRAME_PRIMITIVES_ALLOC`, crash2-decomp a3ccf38) added
      first the lists of the nodes around (decoded by the game's decoder: its backward decoding undoes a delta's swaps
      on the list it starts from, and from the game's own list it scrambled it, its memory and textures then
      overwritten; from a list not the node's own it can loop for ever, `slst_merge`), then the loaded worlds'
      polygons beside the 4:3 picture, projected as the GTE will (a world's header counts its vertices, triangles and
      quads, words 4 to 6), placed by depth (a list goes from the farthest polygon to the nearest and `link_primitive`
      links none nearer than those before it), and those reaching behind the camera cut by the host into triangles of
      worlds it made in the scratchpad's free slots. The edges filled, but artefacts stayed at them and within, and
      on Snow Go's bonus the camera stopped for good with every setting tried (60 frames or not, 16:9 or not), not
      reproduced here: all of it was removed, the decomp's hooks left unused.
13. **The frame rate (done, 60 as an option).** The game's logic stays at its 30 frames a second, as on the
    PlayStation, and a frame of the picture alone is drawn between two of its (`src/between.c`, decomp's
    `FRAME_BETWEEN`, `FRAME_BETWEEN_SHOWN` and `FRAME_ENDED`, crash2-decomp d7053e9): at a game frame's end the camera
    and each object (position, angles, scale) are set halfway from the frame before, a loop of `Sys_MainLoop` runs the
    game's own drawing (the view, the world, the objects with none of their logic, the fade and bands of
    `Gfx_EndFrame`) into the buffer being built, and they go back. The host shows the frames in their order, the one
    between and then the game's, a vertical blank each. The models' animations step at 30.
    - **First tried:** each game frame a vertical blank long, the game measuring a frame's length in blanks. The motion
      scaled, but the GOOL code counts by its runs: twice the sounds (one 20 times where 30 frames a second play it
      11), some never heard at 30, the menus and the memory card's screens twice as fast. Left out.
    - **Checked:** with it on, the game's objects at Turtle Woods' frames are those of the 30 frames a second run, and
      the memory-card run saves the same card; the camera shown moves on in even steps, the frame between halfway;
      the pause menu's frames alike.
    - **No frame between** where the game draws none (`D_800675AC`, while a level opens) or stops its counters (the
      memory card's screens, which it runs a vertical blank a frame already), nor while it is paused (`g_Paused`, its
      opening and closing too): the pause screen's picture shrinks into its corner from the frame shown before, and a
      frame between, its scene drawn anew, alternated with it each vertical blank, a stutter as it opened and closed.
      A frame is shown at its display environment's corner: the drawing area's, 12 lines lower on the memory card's
      screens, shook the picture.
14. **Alongside.** The CD's timing; the game's C at -O2 (its undefined behaviour found); Windows itself.
    - **The game's C at -O2 (done):** each of the 33 files alone at -O2 against the -O0 build: three parted from it,
      `800123F0.c` (the demo's hashes from game frame 7, then stuck), `800158E0.c` (a crash at start-up) and
      `80029884.c` (from game frame 1,979). `-fno-aggressive-loop-optimizations` put each right, and the functions it
      changes there loop over a structure's last array, declared with one element and indexed past it as the PlayStation's
      code does: `Gfx_Init`'s over `g_FrameBuffers.e` (2 buffers, GCC 14 setting up 1), the paging's over
      `D_80067950.e` and `D_800675B0.e`, `func_8002A554`'s and its neighbours' over `D_8006CB6C.p`. A variable of such a type bounds the loop
      for GCC to the one element. With those four declarations made flexible (`[0]`) in a copy, all 33 files at plain
      -O2 played the demo as the -O0 build; 13 such arrays are in the C, the declarations' fix asked of the
      decompilation. GCC 2.7.2 made no such assumption, so `GAME_SEMANTICS` takes the flag, beside `-fwrapv` and the
      others, and the game's C is built at the build's type's level (RelWithDebInfo's -O2). The demo's 6,000 game frames,
      the memory-card run and a player's recorded run of 4,200 give the same `game_frames.txt`, SPU log and card as at
      -O0, and the native code's 9,566 runs agree with Unicorn; the 32-bit Windows build under Wine, the demo's and the
      card's the same. Headless, the demo takes 7.9 s instead of 10.5. The 64-bit build's module keeps the game's C at
      -O0.
15. **Players' bugs, by their recorded runs.** A player records a run in the window (`CRASH2_GAME_CLOCK_RECORD`,
    CLAUDE.md) and it plays again headless frame for frame. Found so far: on Snow Go's bonus, dropped into from above,
    the camera stopped at its vertical path's end for good: `func_800225C0`'s equivalent C held the path's state in 4
    words where `func_80024658` writes 8, the retail frame's spare words under them, the host's frame the player's
    position (crash2-decomp 5a565c4). Paths where the retail code reads what its callers left (a callee-saved register,
    a stack slot), which equivalent C has no value for, call decomp's `CALLER_LEFT`, which the port logs once per
    function (crash2-decomp 8e80c4a): of the six such functions, only `func_8002DFA8` ran on the demo, the card's and
    the Snow Go run, and on every call, its first row comparing with the `$s1` `func_8002E280` leaves, its search's last
    `ny`, now passed to it on the host (`PASSED_ON`, crash2-decomp 30ced94); then none on those runs nor on run_game's
    new-game and turtle-woods schedules played on the port. The same, found by reading: four callers gave
    `Ent_GetProperty`'s interpolated row a word where the retail frames hold two and the row takes up to 8 bytes; in
    `func_800224A0` (the camera's pull on Z, where its path has property 0x252) the host's frame put `new_var` on the
    row's third value, so `scale` was always the second's low half (crash2-decomp f375744). The demo's, the card's and
    the Snow Go run's frames, sound and card are the same with it: none of them reaches the property. The
    decompilation's PCSX-Redux runs don't follow such a recording (the PlayStation's disc reads in real time move its
    game frames from the opening's screens on). Some textures flickered, at 30 frames a second and 4:3 too, at 8 times
    the resolution with WOBBLE and WARPING off: polygons falling back from the steady ones as their vertices crossed the
    1.5 pixels' tolerance (milestone 11, "Kept within the GTE's limits"); whether that was all of it is for the player's
    eye.
16. **64-bit through wasm (done on Linux and Windows; macOS built, not played).** The game's C needs 32-bit pointers
    (decision 1), which macOS and a 64-bit system's libraries no longer give: wasm32 keeps them, and wasm2c turns a wasm
    module into C any 64-bit system compiles. On a 64-bit target CMake builds that way (`cmake/host64.cmake`):
    `tools/wasm/module.py` compiles the game's C and the host's, all but the GPU renderer, with wasi-sdk 34's clang for
    wasm32, links them at the game's addresses and turns the module into C with WABT 1.0.42's wasm2c (both fetched,
    pinned by SHA-256, for Linux x86-64 and ARM, macOS on Apple silicon and Windows x86-64); the program compiles it
    with `src/host64` (WASI's files and clocks, SDL for the module, `main`) and the GPU renderer, `src/render.c`,
    natively on 64-bit SDL3. On Linux x86-64 the demo's 6,000 game frames, the card's run and the Snow Go run write the
    same `game_frames.txt`, SPU log and card as the 32-bit build, the GPU renderer's pictures at 8× are the same, and so
    are a run at 60 frames a second and 16:9 and windowed runs (software and GPU) on Xvfb; some 10% sooner (9.3 s for
    the demo, 10.7 s). `tools/linux/build.sh` also makes `crash2-pc-linux-x86_64.AppImage`, whose GPU renderer takes the
    system's 64-bit Vulkan.
    - **The data's layout:** wasm-ld has no linker script. The split's data and bss are one IR global each
      (`wasm_data.py`), every value at its retail offset and every label an alias into it, the numbers zeros that
      `src/retail_data.c` fills from the disc; between them the game's files' variables, each in a data section of its
      own and 4-aligned in the IR (`align4.py`), tentative definitions left to the split's bss (`uncommon.py`, wasm has
      no common symbols). `module.py` checks each variable at its retail address plus one offset from the link's map.
      The game's memory starts at 0x80070000: above 0x80000000 for its pointers' sign, above the retail data's
      addresses, which `mips.c` maps to the port's copy (below them, the port's own data was taken for the retail's,
      and libsnd's voices written 0xB008 bytes away).
    - **Calls:** a wasm call of another signature than its callee's traps. The game's C has 131 direct calls whose
      arguments or result differ from the definition's (`RotTrans`'s fourth argument, `func_8002CA1C`'s `$a0`, a
      `void` read as `int`...) and 41 indirect ones through pointers to functions of other signatures (`D_8005B1E0`'s
      handlers, called as `void (*)(void)`). `fixcalls.py` gives each direct call its callee's signature, converting
      its arguments and result as a prototype would, and routes each indirect one through a dispatcher of its
      signature, which calls the 192 functions whose address is taken directly when the pointer is theirs. The
      decompilation's own fixes came first (crash2-decomp e068099, 72ac35d, a6ae5e6): `func_80023D7C`'s value, which
      its callers read and x86's leftover `eax` happened to hold (in wasm the projection's distance was 0 and nothing
      was drawn), 17 functions called before their declaration, and functions declared as data for their addresses.
    - **The boundary:** the module imports SDL's functions it calls (39) and the GPU renderer's (13), which
      `src/host64/bridge.c` gives on the module's addresses: SDL's objects are small blocks of the module's memory
      (a texture's holds the width and height `video.c` reads), the events copied as their 128 bytes, a file dialog's
      answer handed to the module on its own thread. The renderer reads the settings, the VRAM, the display area and
      the steady polygons' projections through the module's exports (`Host_Setting`, `Host_Vram`, ...), which the
      32-bit build has as plain functions. A module's function pointer is its table index, so `host_text.h`'s test of
      a word holding one of the port's functions (`mips.c`'s `retail_word`) takes a small number; the module has no
      page protections and no low RAM handler (its address 0 is memory).
    - **Windows:** the same build cross-compiled with MinGW-w64's 64-bit GCC (`cmake/mingw-x86_64.cmake`,
      `tools/windows/build.sh`). `wasi.c` opens and stats files through the C runtime's UTF-16 functions and reads
      Windows' own clocks; the arguments and the environment reach the module in UTF-8, converted from Windows' UTF-16
      (the code page's would garble a path such as `C:\Users\João`); the preopened root is a slot only, answered as a
      directory (wasi-libc reads its rights before each open). Under Wine the demo, card and Snow Go runs write the
      same files as Linux's, a disc under a folder named with accents opens, and the GPU renderer's picture at 4× on
      lavapipe is Linux's to the pixel.
    - **macOS:** SDL_GPU's Metal takes MSL: `tools/msl_shader.py` translates each shader's SPIR-V with SPIRV-Cross, its
      resources at their SPIR-V bindings' indices, where SDL_GPU binds them, and `render.c` gives it a device that
      takes MSL. The split needs the player's disc, which GitHub's runners haven't: the module is made on Linux and
      put in a draft release, and `.github/workflows/macos.yml` builds the app around it (`-DCRASH2_MODULE_DIR`) for
      arm64 and x86_64, macOS 10.15 on, signed ad hoc (`tools/macos/bundle.sh`). There the MSL goes through Metal's
      compiler and the program starts, natively and under Rosetta, until it asks for a disc. Left: played on a Mac.
    - **Licences:** the program carries wasm2c's runtime (WABT, Apache-2.0) and wasi-libc (with musl's, cloudlibc's
      and musl-fts's notices), whose binary releases hold no licence: `cmake/host64.cmake` fetches the texts, pinned
      by SHA-256, and the zips, the AppImages and the Mac app carry them.
