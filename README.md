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

On Debian or Ubuntu, with GCC's 32-bit support, CMake, Ninja, Python 3, glslang (the GPU renderer's shaders), and the
32-bit libraries SDL3 loads for a Wayland window and Vulkan (the i386 architecture enabled):

```sh
sudo dpkg --add-architecture i386 && sudo apt update
sudo apt install gcc-multilib g++-multilib cmake ninja-build python3-venv glslang-tools \
    libwayland-client0:i386 libwayland-egl1:i386 libwayland-cursor0:i386 libxkbcommon0:i386 libegl1:i386 \
    libvulkan1:i386 mesa-vulkan-drivers:i386
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

### Linux's AppImage

`tools/linux/build.sh` builds the same in a container, with podman or docker, into
`build-linux/crash2-pc-linux.AppImage`: the 32-bit program with every library it loads, its C library included, so it
runs on a 64-bit Linux with no 32-bit library installed, X11 or Wayland, PulseAudio or PipeWire. Open it, or run
`./crash2-pc-linux.AppImage "path/to/the.cue"`.

### Windows

The Windows build is cross-compiled on Linux, in a container that holds its tools (MinGW-w64's 32-bit GCC, CMake and
Wine), so nothing is installed on the machine: with podman or docker, once the decompilation is split as above,

```sh
tools/windows/build.sh
```

makes `build-windows/crash2-pc-windows.zip`: `crash2.exe`, `crash2-game.dll`, `SDL3.dll` and the licences. On Windows,
open `crash2.exe`, or drop the disc image's `.cue` onto it. It needs 64-bit Windows: the game sits at 0x80000000,
which only a 64-bit system gives a 32-bit program. Tried under Wine so far, where it plays
the PlayStation's frames as the Linux build does, not on Windows itself.

## Playing

You need your own disc image of the game (USA, SCUS-94154): a `.cue` with its `.bin`, or an `.iso`. Name it as the
program's argument, or start the program without one: it asks for the image in the system's file dialog the first
time and opens the same one after that (its path is kept in `disc.txt` beside the memory card). `CRASH2_DISC` names one
too.

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

The port's options are in the game's menus: OPTIONS on the title screen, and PC OPTIONS in the pause menu's OPTIONS
(Start, then OPTIONS). Up and down choose a row, ✕, left or right change it, DONE, △ or ◯ go back. They are kept
in `settings.ini` beside the memory card:

| Row | What it does |
| --- | --- |
| SCREEN | the window, or the full screen |
| SHAPE | 4:3, stretched to the window, 16:9 (wider; the world comes and goes at its edges) or ZOOM (4:3 cut to 16:9) |
| SMOOTH | the picture's pixels sharp, or smoothed |
| RES | the resolution: the PlayStation's, or 2 to 8 times it, in true colour |
| WOBBLE | the PlayStation's polygons snapped to whole pixels, so they shake as they move; off, they glide |
| WARPING | the PlayStation's textures stretched without depth, so they bend on a slanted surface; off, they lie flat |
| FPS | 30 frames a second as the PlayStation's game runs, or 60: a frame drawn between each two, halfway |

The resolution takes Vulkan from a 32-bit driver: on Linux the i386 Vulkan packages (`libvulkan1:i386` and
`mesa-vulkan-drivers:i386` on Debian, what Steam installs too); on Windows the graphics card's driver. Without it the
game shows the PlayStation's picture, and the options leave out the resolution, the steadiness and the perspective.

The memory card is `memcard1.mcd` in SDL's preference folder (on Linux under `~/.local/share`, on Windows under
`%APPDATA%`): a 128 KB card image, the format emulators and card tools use, so a save moves between them.
`CRASH2_MEMORY_CARD=FILE` uses another.

## How it's made: AI-assisted

This project uses AI, openly: its code and docs are written with AI assistance (Claude, run through Claude Code),
directed and reviewed by the maintainer. The game's C comes from the decompilation, where matched C is proven equal to
the retail code and equivalent C is tested against it; the port is checked against the PlayStation build running the
same C.

## Licence

GPL-3.0 ([LICENSE](LICENSE)), as the decompilation it is built from. `third_party/` holds others' code under licences
compatible with it, each with its origin and changes ([third_party/swanstation](third_party/swanstation/README.md)).
