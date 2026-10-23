# CLAUDE.md

A native PC port of Crash Bandicoot 2 (PlayStation, USA, SCUS-94154), built from the C of crash2-decomp, a git submodule
at `decomp/`. Read before working: README.md and docs/ROADMAP.md (the decisions taken and the milestones, in order).
A session takes the first milestone not done and updates docs/ROADMAP.md with what it did.

## Hard rules

- **The game's C lives only in the decompilation.** Never copy its C here, never edit `decomp/` in place. A change the
  port needs in that C is made in crash2-decomp under its own rules (the PlayStation executable byte for byte the same,
  both of its builds green), pushed there, then the submodule moves to that commit here. The port redefines the
  decompilation's macros; it doesn't patch its files.
- **Never commit game data.** Not the executable, not the disc's files, not anything generated from them (the split's
  data, extracted textures or sounds). The build generates them from the player's own disc image into `build/`.
- **Never commit a path of this machine** (an absolute path, a home folder): not in code, docs or build files. Run
  `git grep -nF "$PWD"` from the root before every push.
- **Don't copy code from projects without a compatible licence.** The PsyQ libraries are Sony's: the port writes the
  functions the game calls from their documented behaviour, never from their code. libsnd and libspu run instead as the
  retail executable's own machine code, read from the player's disc at start-up into the interpreter (`src/sdk.c`,
  decided 2026-10-22): nothing of them is in the repository, as the game's data isn't. Emulators are references for the
  hardware's behaviour; their code comes in only under a licence compatible with GPL-3.0, into `third_party/NAME/`
  with its licence, a README naming its origin (repository and commit) and changes, and a notice on each file
  changed.
- **The PlayStation build is the reference.** A difference in what the port shows or computes is checked against the
  decompilation's equivalent build running the same C in an emulator before it is called a bug of either.

## Building

README.md, "Building": the decompilation split in `decomp/` (its own `.venv`, ignored there), then
`cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i686.cmake` and `cmake --build build`; `build/crash2`
runs it. GCC with 32-bit support (`gcc-multilib` on Debian). CMake fetches SDL3 3.2.10 (pinned by SHA-256) and builds it
for 32-bit x86: Debian's i386 package conflicts with backports' Mesa. The build is RelWithDebInfo unless asked
otherwise, the game's C always at -O0 (docs/ROADMAP.md, milestone 6, "Speed"); a change that should leave the game as it
is shows it by the same `game_frames.txt` hashes as before.

## Running and testing

- `build/crash2 DISC.cue` runs headless; `tools/check_layout.py` checks the game's data layout after each link.
- The native code's test: `CRASH2_NATIVE_TRACE=build/native.trace build/crash2 DISC.cue` for a minute, then
  `.venv/bin/python tools/check_native.py build/native.trace` (`.venv` from `tools/requirements.txt`).
- Against the PS1: decomp's `tools/run_game.py DISC.cue --schedule demo --against retail --dump N` (in the
  decompilation, one run at a time) dumps the PS1's RAM at game frame N into `build/run_game/retail/ram_N.bin`. The
  port's `CRASH2_HEADLESS=1 CRASH2_GAME_CLOCK=DIR CRASH2_GAME_CLOCK_LAST=N CRASH2_GAME_CLOCK_DUMPS=N build/crash2
  DISC.cue` dumps its own, and `tools/compare_ram.py PS1.bin DIR/ram_N.bin build/crash2 decomp` names the variables
  that differ.
- Presses on the game clock: `CRASH2_GAME_CLOCK_PRESSES=FILE` takes a run_game schedule's press lines (decomp's
  `build/run_game/NAME/schedule.txt`), so the port plays the PS1's run. The memory card against the PS1: decomp's
  `tools/run_game.py DISC.cue --schedule memory-card --against retail` saves on an empty card
  (`build/run_game/retail/memcard1.mcd`); the port with that schedule's presses and `CRASH2_MEMORY_CARD=FILE` on a new
  file (3,500 game frames) saves its own, to compare frame by frame.
- The GPU renderer headless: `CRASH2_HEADLESS_GPU=1` beside `CRASH2_HEADLESS=1` draws on SDL's offscreen driver,
  `CRASH2_SETTINGS=FILE` names a settings file (`resolution=3` is 4×, `steady=1` and `perspective=1` the steady
  polygons; headless reads none otherwise), and each
  `CRASH2_GAME_CLOCK_SHOTS` frame also writes `DIR/shot_N_gpu.ppm` at the resolution. `VK_ICD_FILENAMES` picks the
  driver: `/usr/share/vulkan/icd.d/lvp_icd.json` (lavapipe, in software) or `radeon_icd.json`. The game's frames, sound
  and card stay those of the software run: the renderer only draws. `tools/linux/run.sh --gpu --window` runs the
  AppImage's window on lavapipe in the container.
- The game's objects: `CRASH2_GAME_CLOCK_OBJECTS=N,M` lists the live objects at those game frames in
  `DIR/objects_N.txt` (GOOL entry, subtype, state, the code word the pc is at, position and velocity), to find the
  object a change concerns or follow one (Crash is `WillC`). `fps60=1` in a `CRASH2_SETTINGS` file draws a frame
  between each two of the game's: the game's own frames and state stay those of a run without it (the frames between
  count none). `CRASH2_DUMP=DIR CRASH2_DUMP_EVERY=1 CRASH2_DUMP_FROM=N` writes each picture shown from blank N (a
  picture 0.4 MB: a narrow window of blanks, out of a tmpfs).
- A player's run played again: `CRASH2_GAME_CLOCK=DIR CRASH2_GAME_CLOCK_RECORD=FILE` in the window (with
  `CRASH2_MEMORY_CARD` on a new file, the settings unchanged during the run) records the pad, read once a game frame,
  as press lines; `CRASH2_GAME_CLOCK_PRESSES=FILE` headless, with the same settings and a new card, runs the same game
  frames (a windowed run of a schedule writes the same `game_frames.txt` as a headless one).
- The sound against the PS1: `tools/spu_log.lua` in decomp's PCSX-Redux logs the PS1's SPU register writes (its header
  has the command), `CRASH2_SPU_LOG=FILE` the port's, and `tools/compare_spu.py PS1.log PORT.log --voices 16` aligns
  their notes and lists each note missing on one side or differing.
- The AppImage: `tools/linux/build.sh` builds `build-linux/` and `build-linux/crash2-pc-linux.AppImage` in the container
  (`tools/linux/Containerfile`); `tools/linux/run.sh [--window] DISC.cue OUT NAME=VALUE...` runs it in a 64-bit
  system with no 32-bit library, `/out` being OUT, for the same checks.
- Windows: `tools/windows/build.sh` builds `build-windows/` and its zip in the container
  (`tools/windows/Containerfile`); `tools/windows/run.sh [--window] DISC.cue OUT NAME=VALUE...` runs it under Wine,
  `Z:/out` being OUT. The Windows build's `game_frames.txt`, SPU log and card match the Linux build's to the byte on
  the same run; a change to the host's code shows it on both.
- Debugging: `CRASH2_SAMPLE=1` samples where the time goes; a fatal error aborts, and gdb reads its core dump
  (`coredumpctl dump`). A live gdb can't follow the game past the low RAM's single steps (`src/low_ram.c`).

## Code

C for what the PlayStation did, in the decompilation's style: exhaustive branching (every `if`/`else if` chain ends with
an `else`, every `switch` has a `default`), no nested ternaries, named constants, succinct impersonal comments,
`TRADEOFF:` comments for deliberate simplifications. Docs record findings with their evidence, in English.
