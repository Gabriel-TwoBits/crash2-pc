#ifndef HOST_TEXT_H
#define HOST_TEXT_H

#include <stdint.h>

/* The port's own code, the game's C among it: from the program's start to its text's end, as the linker names them on
 * Linux and on Windows */
#ifdef _WIN32
extern char __ImageBase[];
extern char etext[];
#define HOST_TEXT_START __ImageBase
#define HOST_TEXT_END etext
#else
extern char __executable_start[];
extern char __etext[];
#define HOST_TEXT_START __executable_start
#define HOST_TEXT_END __etext
#endif

#ifdef __wasi__
/* Spike: a module's function pointer is its index in the function table, a small number */
#define WASM_TABLE_LIMIT 0x10000
static inline int Host_InOwnCode(uint32_t address) {
    return address != 0 && address < WASM_TABLE_LIMIT;
}
#else
static inline int Host_InOwnCode(uint32_t address) {
    return address >= (uint32_t)(uintptr_t)HOST_TEXT_START && address < (uint32_t)(uintptr_t)HOST_TEXT_END;
}
#endif

#endif /* HOST_TEXT_H */
