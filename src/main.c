#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#elif !defined(__wasi__)
#include <sys/mman.h>
#endif

#include "disc.h"
#include "host.h"
#include "host_log.h"
#include "low_ram.h"
#include "psyq/host_psyq.h"
#include "sampler.h"
#include "settings.h"

/* What the PlayStation's start-up (libsn's __SN_ENTRY_POINT) leaves the game: a heap from the image's end to the stack,
 * 8 bytes under the top of the 2 MB of RAM and 4 KB deep (the executable's words D_8005F188 and D_8005F184) */
#define PS1_RAM_BYTES 0x200000
#define PS1_STACK_BYTES 0x1000
#define PS1_STACK_TOP_GAP 8
#define PS1_IMAGE_END 0x6F1F0
#define HEAP_BYTES (PS1_RAM_BYTES - PS1_STACK_TOP_GAP - PS1_STACK_BYTES - PS1_IMAGE_END)
/* The start-up passes the heap 4 bytes in; func_8001144C marks a word past its end */
#define HEAP_START_OFFSET 4
#define HEAP_SLACK_BYTES 8
#define SCRATCHPAD_BYTES 0x400
/* The heap at the retail heap's place modulo 64 KB (libsn's start-up puts its base at the image's end, 0x8006F1F0),
 * and the scratchpad aligned to its size as at 0x1F800000: the game's alignments of their addresses come out as on
 * the PlayStation (a buffer's padding in Gfx_OpenVideo, D_80069340) */
#define HEAP_ALIGNMENT 0x10000
#define RETAIL_HEAP_BASE 0x8006F1F0u
#define HEAP_PLACE (RETAIL_HEAP_BASE & (HEAP_ALIGNMENT - 1))
#ifdef _WIN32
/* COFF aligns a variable to 8 KB at most: the heap's area holds a 64 KB alignment's slack instead */
#define HEAP_AREA_ALIGNMENT 1
#define HEAP_AREA_SLACK HEAP_ALIGNMENT
#else
#define HEAP_AREA_ALIGNMENT HEAP_ALIGNMENT
#define HEAP_AREA_SLACK 0
#endif

/* The heap's base and size, the start-up's words libsn's InitHeap reads */
extern unsigned char *D_8005CB28;
extern unsigned int D_8005CB2C;
void func_8001144C(int *start, unsigned int bytes);
int Game_Main(void);

/* The linker's: the program's first address */
#ifdef _WIN32
extern IMAGE_DOS_HEADER __ImageBase;
#define PROGRAM_START ((unsigned int)(uintptr_t)&__ImageBase)
#else
extern char __executable_start[];
#define PROGRAM_START ((unsigned int)(uintptr_t)__executable_start)
#endif

unsigned char g_HostScratchpad[SCRATCHPAD_BYTES] __attribute__((aligned(SCRATCHPAD_BYTES)));
static unsigned char s_HeapArea[HEAP_AREA_SLACK + HEAP_PLACE + HEAP_BYTES + HEAP_SLACK_BYTES]
    __attribute__((aligned(HEAP_AREA_ALIGNMENT)));

/* The heap: HEAP_PLACE past the area's first 64 KB boundary, its start where the area is aligned */
static unsigned char *heap_base(void) {
    uintptr_t boundary = ((uintptr_t)s_HeapArea + HEAP_ALIGNMENT - 1) & ~(uintptr_t)(HEAP_ALIGNMENT - 1);

    return (unsigned char *)boundary + HEAP_PLACE;
}

/* The linker's window reservation past the program (cmake/game_data.ld) */
extern char __window_rest_start[];
extern char __window_rest_end[];

#ifndef __wasi__
/* The program in the window, the window's rest unreadable: the game's access past its image faults (low_ram.c
 * reports it) */
static int protect(void *start, unsigned int bytes) {
#ifdef _WIN32
    DWORD old;

    return VirtualProtect(start, bytes, PAGE_NOACCESS, &old) ? 0 : -1;
#else
    return mprotect(start, bytes, PROT_NONE);
#endif
}

#endif

#ifdef __wasi__
/* The 64-bit build's module: its memory holds the game's from 0x80070000 (tools/wasm/module.py), with no linker script
 * and no pages to protect; its layout is checked from the link's map */
static void check_window(void) {
}
#else
static void check_window(void) {
    unsigned int first = PROGRAM_START;
    unsigned int last = (unsigned int)(uintptr_t)__window_rest_start;
    unsigned int rest = (unsigned int)(__window_rest_end - __window_rest_start);

    if (first < RAM_WINDOW || (unsigned int)(uintptr_t)__window_rest_end != RAM_WINDOW + RAM_WINDOW_BYTES) {
        Host_Fatal("the program spans 0x%08X to 0x%08X, outside the 8 MB window at 0x%08X (src/host.h)", first, last,
                   RAM_WINDOW);
    } else if (protect(__window_rest_start, rest) != 0) {
        Host_Fatal("can't protect the window from 0x%08X", last);
    } else {
        Host_Log("the program spans 0x%08X to 0x%08X, the window reserved past it", first, last);
    }
}
#endif

#ifdef _WIN32
/* On Windows the port is a library at 0x80000000 that crash2.exe loads and enters here (src/launcher.c) */
__declspec(dllexport) int Host_Main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    check_window();
    Host_InstallLowRam();
    Host_StartSampler();
    Host_LoadSettings();
    Host_OpenDisc(Host_ChooseDisc(argc, argv));
    Host_SoundStart();
    D_8005CB28 = heap_base();
    D_8005CB2C = HEAP_BYTES;
    func_8001144C((int *)(heap_base() + HEAP_START_OFFSET), HEAP_BYTES);
    return Game_Main();
}
