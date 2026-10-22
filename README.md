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

The game's C boots on the PC, headless: it reads the disc image, pages its first level in and runs its objects up to
their first native code, against stubs of most of the PlayStation's libraries. The plan and its milestones are in
[docs/ROADMAP.md](docs/ROADMAP.md).

## How it's built

The decompilation is a git submodule (`decomp/`), pinned at a commit, and the only source of the game's C: this
repository holds no copy of it. A change the port needs in that C is made in the decompilation, where the PlayStation
executable must stay byte for byte the same, and the submodule moves to it. So the PlayStation build stays the reference
the port is compared against.

## Building

On Debian or Ubuntu, with GCC's 32-bit support, CMake, Ninja and Python 3:

```sh
sudo apt install gcc-multilib cmake ninja-build python3-venv
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

## How it's made: AI-assisted

This project uses AI, openly: its code and docs are written with AI assistance (Claude, run through Claude Code),
directed and reviewed by the maintainer. The game's C comes from the decompilation, where matched C is proven equal to
the retail code and equivalent C is tested against it; the port is checked against the PlayStation build running the
same C.

## Licence

GPL-3.0 ([LICENSE](LICENSE)), as the decompilation it is built from.
