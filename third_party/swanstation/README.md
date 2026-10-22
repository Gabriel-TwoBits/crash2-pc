# SwanStation's software rasterizer

From [libretro/swanstation](https://github.com/libretro/swanstation) at commit
`cc94ebcd38a88a0e90788d7c8bcd764a009a5fa0`, under the GNU GPL version 3 (LICENSE), by Connor McLaughlin and
SwanStation's contributors. The rasterizer itself credits Mednafen's for its polygons and lines.

crash2-pc uses its GPU software backend to draw the GPU's primitives (`src/gpu_raster.cpp` hands them over), and its
SPU for the sound (`src/spu_host.cpp` drives it, against stand-ins for the emulator's other parts in `src/spu_shim/`).

The files, from SwanStation's `src/`:

- `common/bitfield.h`, `common/fifo_queue.h`, `common/rectangle.h`, `common/types.h`, `core/types.h`,
  `core/gpu_types.h`: unchanged.
- `core/gpu_sw_backend.h`: draws into the host's VRAM through `m_vram_ptr`, in place of an array of its own; its
  drawing functions are public.
- `core/gpu_sw_backend.cpp`: without `host_display.h` and `system.h`; the VRAM isn't its own, cleared through
  `m_vram_ptr`.
- `core/gpu_backend.h`: rewritten. The base class keeps only the VRAM pointer and the drawing area, its commands called
  directly, with no command queue or GPU thread.
- `core/spu.h`, `core/spu.cpp`: `AddTicks`, for the host to give it each frame's CPU cycles, and `WriteRAM`, for
  libspu's transfers into its RAM at once.
