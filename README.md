# Crash Bandicoot 2 for PC

A native PC port of **Crash Bandicoot 2: Cortex Strikes Back** (PlayStation, USA, SCUS-94154), built from the C of
[crash2-decomp](https://github.com/Gabriel-TwoBits/crash2-decomp). The game's code is the decompilation's, compiled for
the PC; this repository holds what the PlayStation did around it: the window, the renderer of the GPU's packets, the
sound, the CD, the memory card and the pad.

> [!IMPORTANT]
> **No game data is included.** You need your own copy of the game: the build reads the executable's layout from your
> disc image, and the program reads the executable's data and the game's levels from it when it starts. Crash
> Bandicoot is a trademark of Activision; PlayStation of Sony Interactive Entertainment. This is a fan project, not
> affiliated with or endorsed by either.

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

### The 64-bit build

The game's C needs 32-bit pointers. On a 64-bit system without a 32-bit toolchain, CMake builds it through
WebAssembly instead: the game's C and the port's, but the GPU renderer, are compiled for wasm32, whose pointers are 32
bits, and wasm2c turns that module into C the system's compiler builds natively, with SDL and the GPU renderer around
it (docs/ROADMAP.md, milestone 16). It plays the same frames as the 32-bit build, takes the system's own 64-bit Vulkan
for the GPU renderer, and needs no 32-bit library at all. CMake fetches wasi-sdk 34 and WABT 1.0.42, pinned by
SHA-256, for Linux (x86-64 or ARM), macOS on Apple silicon and Windows on x86-64:

```sh
sudo apt install gcc g++ cmake ninja-build python3 glslang-tools libx11-dev libxext-dev libxcursor-dev libxi-dev \
    libxfixes-dev libxrandr-dev libxss-dev libxkbcommon-dev libwayland-dev libegl-dev libpulse-dev libasound2-dev \
    libudev-dev
cmake -S . -B build64 -G Ninja
cmake --build build64
build64/crash2 "path/to/Crash Bandicoot 2 - Cortex Strikes Back (USA).cue"
```

Checked on Linux x86-64, and for Windows x86-64 under Wine (below): the same frames, sound and saves as the 32-bit
build, and the same pictures from the GPU renderer. On macOS the GPU renderer takes its shaders in Metal's MSL,
translated from their SPIR-V by SPIRV-Cross (Homebrew's `spirv-cross` and `glslang`). The Mac app is built on GitHub's
macOS runner (`.github/workflows/macos.yml`) for Apple silicon and Intel, macOS 10.15 on, around a module made on Linux
(`-DCRASH2_MODULE_DIR`), since the split needs your disc: there Metal's compiler takes the shaders and the program
starts on both, but it hasn't been played on a Mac yet.

### Linux's AppImage

`tools/linux/build.sh` builds the same in a container, with podman or docker, into
`build-linux/crash2-pc-linux.AppImage`: the 32-bit program with every library it loads, its C library included, so it
runs on a 64-bit Linux with no 32-bit library installed, X11 or Wayland, PulseAudio or PipeWire. Open it, or run
`./crash2-pc-linux.AppImage "path/to/the.cue"`. It also builds the 64-bit build the same way into
`build-linux64/crash2-pc-linux-x86_64.AppImage`, whose GPU renderer takes the system's 64-bit Vulkan.

### Windows

The Windows builds are cross-compiled on Linux, in a container that holds their tools (MinGW-w64's GCC, CMake and
Wine), so nothing is installed on the machine: with podman or docker, once the decompilation is split as above,

```sh
tools/windows/build.sh
```

makes `build-windows64/crash2-pc-windows-x86_64.zip`, the 64-bit build (`crash2.exe`, `SDL3.dll` and the licences),
and `build-windows/crash2-pc-windows.zip`, the 32-bit one (`crash2.exe`, `crash2-game.dll`, `SDL3.dll` and the
licences). On Windows, open `crash2.exe`, or drop the disc image's `.cue` onto it. Both need 64-bit Windows: the
32-bit build's game sits at 0x80000000, which only a 64-bit system gives a 32-bit program. Tried under Wine so far,
where both play the PlayStation's frames as the Linux build does, not on Windows itself.

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

The resolution takes Vulkan from the system's driver: the 64-bit build from its 64-bit one; the 32-bit build on Linux
from the i386 Vulkan packages (`libvulkan1:i386` and `mesa-vulkan-drivers:i386` on Debian, what Steam installs too), on
Windows from the graphics card's driver. On macOS it takes Metal. Without them the game shows the PlayStation's
picture, and the options leave out the resolution, the steadiness and the perspective.

The Mac app is signed ad hoc, without an Apple Developer ID: the first time, macOS refuses to open it, and System
Settings, Privacy & Security, "Open Anyway" allows it from then on.

The memory card is `memcard1.mcd` in SDL's preference folder (on Linux under `~/.local/share`, on Windows under
`%APPDATA%`, on macOS under `~/Library/Application Support`): a 128 KB card image, the format emulators and card
tools use, so a save moves between them. `CRASH2_MEMORY_CARD=FILE` uses another.

## How it's made: AI-assisted

This project uses AI, openly: its code and docs are written with AI assistance (Claude, run through Claude Code),
directed and reviewed by the maintainer. The game's C comes from the decompilation, where matched C is proven equal to
the retail code and equivalent C is tested against it; the port is checked against the PlayStation build running the
same C.

## Licence

GPL-3.0 ([LICENSE](LICENSE)), as the decompilation it is built from. `third_party/` holds others' code under licences
compatible with it, each with its origin and changes ([third_party/swanstation](third_party/swanstation/README.md)).
