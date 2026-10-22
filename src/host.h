#ifndef HOST_H
#define HOST_H

/* Included before each of the decompilation's files: the host's definitions of its platform macros
 * (decomp/include/platform.h, decomp/include/gte.h, decomp/docs/ROADMAP.md section 6, "The macros") */

/* The program, its data and the game's heap sit in the 16 MB window at RAM_WINDOW (main.c checks it at start-up):
 * the ordering table's 24-bit links (OT_LINK's default) hold, and a pointer into the window is the game's own */
#define RAM_WINDOW 0x08000000
#define RAM_WINDOW_MASK 0xFF000003
#define RAM_BASE RAM_WINDOW
#define RAM_ADDRESS(value) (((unsigned int)(value) & RAM_WINDOW_MASK) == RAM_WINDOW)

extern unsigned char g_HostScratchpad[];
#define SCRATCHPAD ((unsigned int)g_HostScratchpad)

/* The GTE's registers, data then control, in the host's memory (gte.c) */
extern unsigned int g_HostGte[];
#define HOST_GTE_CONTROL 32
#define GTE_READ_DATA(reg, out) ((out) = g_HostGte[reg])
#define GTE_READ_CONTROL(reg, out) ((out) = g_HostGte[HOST_GTE_CONTROL + (reg)])
#define GTE_WRITE_DATA(reg, value) (g_HostGte[reg] = (unsigned int)(value))
#define GTE_WRITE_CONTROL(reg, value) (g_HostGte[HOST_GTE_CONTROL + (reg)] = (unsigned int)(value))
#define GTE_STORE_DATA(reg, lvalue) (*(unsigned int *)&(lvalue) = g_HostGte[reg])
#define GTE_LOAD_DATA(reg, lvalue) (g_HostGte[reg] = *(unsigned int *)&(lvalue))

/* Operation 73's native code (native.c) */
#define GOOL_RUN_NATIVE Host_RunNative
struct GoolObject;
void Host_RunNative(struct GoolObject *object, int *state_pool, int **pc, int **sp, unsigned char *frame);

#endif /* HOST_H */
