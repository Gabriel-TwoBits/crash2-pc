#include <stdlib.h>
#include <sys/mman.h>

#include "host.h"
#include "host_log.h"
#include "low_ram.h"
#include "psyq/host_psyq.h"
#include "sampler.h"

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

/* The heap's base and size, the start-up's words libsn's InitHeap reads */
extern unsigned char *D_8005CB28;
extern unsigned int D_8005CB2C;
void func_8001144C(int *start, unsigned int bytes);
int Game_Main(void);

/* GNU ld's: the program's first address */
extern char __executable_start[];

unsigned char g_HostScratchpad[SCRATCHPAD_BYTES] __attribute__((aligned(SCRATCHPAD_BYTES)));
static unsigned char s_HeapArea[HEAP_PLACE + HEAP_BYTES + HEAP_SLACK_BYTES] __attribute__((aligned(HEAP_ALIGNMENT)));
unsigned char *const g_HostHeap = s_HeapArea + HEAP_PLACE;

/* The linker's window reservation past the program (cmake/game_data.ld) */
extern char __window_rest_start[];
extern char __window_rest_end[];

/* The program in the window, the window's rest unreadable: the game's access past its image faults (low_ram.c
 * reports it) */
static void check_window(void) {
    unsigned int first = (unsigned int)__executable_start;
    unsigned int last = (unsigned int)__window_rest_start;
    unsigned int rest = (unsigned int)(__window_rest_end - __window_rest_start);

    if (first < RAM_WINDOW || (unsigned int)__window_rest_end != RAM_WINDOW + RAM_WINDOW_BYTES) {
        Host_Fatal("the program spans 0x%08X to 0x%08X, outside the 8 MB window at 0x%08X (src/host.h)", first, last,
                   RAM_WINDOW);
    } else if (mprotect(__window_rest_start, rest, PROT_NONE) != 0) {
        Host_Fatal("can't protect the window from 0x%08X", last);
    } else {
        Host_Log("the program spans 0x%08X to 0x%08X, the window reserved past it", first, last);
    }
}

/* The disc image: the first argument, else CRASH2_DISC */
#define DISC_VARIABLE "CRASH2_DISC"

int main(int argc, char **argv) {
    const char *disc = argc > 1 ? argv[1] : getenv(DISC_VARIABLE);

    check_window();
    Host_InstallLowRam();
    Host_StartSampler();
    if (disc == NULL) {
        Host_Fatal("usage: crash2 DISC.cue (or DISC.iso), or " DISC_VARIABLE " set to it");
    } else {
        Host_OpenDisc(disc);
        Host_SoundStart();
    }
    D_8005CB28 = g_HostHeap;
    D_8005CB2C = HEAP_BYTES;
    func_8001144C((int *)(g_HostHeap + HEAP_START_OFFSET), HEAP_BYTES);
    return Game_Main();
}
