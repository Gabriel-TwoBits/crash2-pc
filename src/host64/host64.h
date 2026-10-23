#ifndef HOST64_H
#define HOST64_H

#include <stdint.h>

#include "crash2_module.h"

/* The 64-bit build's native program (docs/ROADMAP.md, milestone 16): the module wasm2c made of the game's C and the
 * host's (tools/wasm/module.py), and around it what needs a 64-bit system's libraries: SDL's window, sound, pad and
 * dialogs (bridge.c), the GPU renderer (src/render.c), the files and clocks (wasi.c) */

extern w2c_crash2 g_Module;
extern struct w2c_env g_Env;
extern struct w2c_wasi__snapshot__preview1 g_Wasi;

/* The module's bytes at its address, NULL for its 0 */
uint8_t *Module_Bytes(uint32_t address);

void Wasi_Start(int argc, char **argv);

#endif /* HOST64_H */
