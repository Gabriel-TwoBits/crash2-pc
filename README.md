# Crash Bandicoot 2 for PC

A native PC port of **Crash Bandicoot 2: Cortex Strikes Back** (PlayStation, USA, SCUS-94154), built from the C of
[crash2-decomp](https://github.com/Gabriel-TwoBits/crash2-decomp). The game's code is the decompilation's, compiled for
the PC; this repository holds what the PlayStation did around it: the window, the renderer of the GPU's packets, the
sound, the CD, the memory card and the pad.

> [!IMPORTANT]
> **No game data is included.** You need your own copy of the game: the build reads the executable from your disc image,
> and the game reads its levels from it. Crash Bandicoot is a trademark of Activision; PlayStation of Sony Interactive
> Entertainment. This is a fan project, not affiliated with or endorsed by either.

## Status

The game's C runs on the PC from the disc image, in a window drawn by a GPU in software: the Naughty Dog logo, the
title screen, the attract demo, and the game from the keyboard or a gamepad, with its sound effects and music, and its
saves on a memory card in a file. The plan and its milestones are in [docs/ROADMAP.md](docs/ROADMAP.md).

## How it's built

The decompilation is a git submodule (`decomp/`), pinned at a commit, and the only source of the game's C: this
repository holds no copy of it. A change the port needs in that C is made in the decompilation, where the PlayStation
executable must stay byte for byte the same, and the submodule moves to it. So the PlayStation build stays the reference
the port is compared against. The sound libraries, libsnd and libspu, are the one part run as the executable's machine
code: read from your disc image at start-up, on an interpreter of the PlayStation's CPU, so the music and the sound
effects are the PlayStation's own.

## Building

On Debian or Ubuntu, with GCC's 32-bit support, CMake, Ninja, Python 3, and the 32-bit libraries SDL3 loads for a
Wayland window (the i386 architecture enabled):

```sh
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install gcc-multilib g++-multilib cmake ninja-build python3-venv \
    libwayland-client0:i386 libwayland-egl1:i386 libwayland-cursor0:i386 libxkbcommon0:i386 libegl1:i386
git clone --recursive https://github.com/Gabriel-TwoBits/crash2-pc.git
cd crash2-pc/decomp
# The decompilation's split of your executable: its splitter, then your disc image
python3 -m venv .venv
.venv/bin/pip install 'splat64[mips]==0.50.1' spimdisasm==1.42.4 rabbitizer==1.16.2
.venv/bin/python tools/extract_disc.py "path/to/Crash Bandicoot 2 - Cortex Strikes Back (USA).cue"
.venv/bin/python configure.py
cd ..
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i686.cmake
cmake --build build
build/crash2 "path/to/Crash Bandicoot 2 - Cortex Strikes Back (USA).cue"
```

What the split and the build make from your executable stays in `decomp/` (ignored there) and `build/`.

## Playing

| PlayStation | Keyboard | Gamepad |
| --- | --- | --- |
| D-pad | arrows | D-pad, left stick |
| ✕ (jump) | X | south |
| ▢ (spin) | Z | west |
| ◯ | C | east |
| △ | V | north |
| Start | Enter | start |
| Select | Backspace | back |
| L1, R1 | A, S | shoulders |
| L2, R2 | Q, W | triggers |

The memory card is `memcard1.mcd` in SDL's preference folder (on Linux, under `~/.local/share`): a 128 KB card image,
the format emulators and card tools use, so a save moves between them. `CRASH2_MEMORY_CARD=FILE` uses another.

## How it's made: AI-assisted

This project uses AI, openly: its code and docs are written with AI assistance (Claude, run through Claude Code),
directed and reviewed by the maintainer. The game's C comes from the decompilation, where matched C is proven equal to
the retail code and equivalent C is tested against it; the port is checked against the PlayStation build running the
same C.

## Licence

GPL-3.0 ([LICENSE](LICENSE)), as the decompilation it is built from. `third_party/` holds others' code under licences
compatible with it, each with its origin and changes ([third_party/swanstation](third_party/swanstation/README.md)).
