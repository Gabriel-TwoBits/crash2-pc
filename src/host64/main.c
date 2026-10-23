#include <stdio.h>
#include <stdlib.h>

#include "host64.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"

w2c_crash2 g_Module;

/* The module's start: the C library's, then the port's main (src/main.c); a trap ends the program with its reason */
int main(int argc, char **argv) {
    wasm_rt_trap_t trap;

    Wasi_Start(argc, argv);
    wasm_rt_init();
    wasm2c_crash2_instantiate(&g_Module, &g_Env, &g_Wasi);
    trap = wasm_rt_impl_try();
    if (trap == WASM_RT_TRAP_NONE) {
        w2c_crash2_0x5Fstart(&g_Module);
    } else {
        fprintf(stderr, "crash2-pc: fatal: the module trapped: %s\n", wasm_rt_strerror(trap));
        return EXIT_FAILURE;
    }
    wasm2c_crash2_free(&g_Module);
    wasm_rt_free();
    return EXIT_SUCCESS;
}
