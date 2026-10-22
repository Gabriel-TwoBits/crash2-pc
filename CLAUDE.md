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
  functions the game calls from their documented behaviour, never from their code. Emulators are references for the
  hardware's behaviour; their code comes in only under a licence compatible with GPL-3.0, into `third_party/NAME/`
  with its licence, a README naming its origin (repository and commit) and changes, and a notice on each file
  changed.
- **The PlayStation build is the reference.** A difference in what the port shows or computes is checked against the
  decompilation's equivalent build running the same C in an emulator before it is called a bug of either.

## Building

README.md, "Building": the decompilation split in `decomp/` (its own `.venv`, ignored there), then
`cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i686.cmake` and `cmake --build build`; `build/crash2`
runs it. GCC with 32-bit support (`gcc-multilib` on Debian). CMake fetches SDL3 3.2.10 (pinned by SHA-256) and builds it
for 32-bit x86: Debian's i386 package conflicts with backports' Mesa.

## Running and testing

- `build/crash2 DISC.cue` runs headless; `tools/check_layout.py` checks the game's data layout after each link.
- The native code's test: `CRASH2_NATIVE_TRACE=build/native.trace build/crash2 DISC.cue` for a minute, then
  `.venv/bin/python tools/check_native.py build/native.trace` (`.venv` from `tools/requirements.txt`).
- Debugging: `CRASH2_SAMPLE=1` samples where the time goes; a fatal error aborts, and gdb reads its core dump
  (`coredumpctl dump`). A live gdb can't follow the game past the low RAM's single steps (`src/low_ram.c`).

## Code

C for what the PlayStation did, in the decompilation's style: exhaustive branching (every `if`/`else if` chain ends with
an `else`, every `switch` has a `default`), no nested ternaries, named constants, succinct impersonal comments,
`TRADEOFF:` comments for deliberate simplifications. Docs record findings with their evidence, in English.
