#ifndef HOST_H
#define HOST_H

/* Included before each of the decompilation's files: the host's definitions of its platform macros
 * (decomp/include/platform.h, decomp/include/gte.h, decomp/docs/ROADMAP.md section 6, "The macros") */

/* The program, its data and the game's heap sit where the PlayStation's RAM does, in KSEG0's first 8 MB: CMakeLists.txt
 * links it there, main.c checks it at start-up. The game tells a pointer from an error by its sign (a pointer is below
 * -0xFE) as well as by RAM_ADDRESS, and links the ordering table by 24 bits: the PlayStation's RAM_BASE, RAM_ADDRESS
 * and OT_LINK hold as they are (docs/ROADMAP.md, decision 2) */
#define RAM_WINDOW 0x80000000
#define RAM_WINDOW_BYTES 0x800000

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

/* The arguments the PlayStation's code passes on in registers, unnamed there */
#define PASSED_ON(arguments...) arguments

/* Operation 73's native code (native.c) */
#define GOOL_RUN_NATIVE Host_RunNative
struct GoolObject;
void Host_RunNative(struct GoolObject *object, int *state_pool, int **pc, int **sp, unsigned char *frame);

#endif /* HOST_H */
