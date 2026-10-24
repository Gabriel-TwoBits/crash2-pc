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

/* A C symbol's name for the assembler, with the target's prefix (32-bit Windows' underscore, none on Linux) */
#define HOST_STRING(text) #text
#define HOST_EXPANDED_STRING(text) HOST_STRING(text)
#define ASM_SYMBOL(name) HOST_EXPANDED_STRING(__USER_LABEL_PREFIX__) #name

/* The arguments the PlayStation's code passes on in registers, unnamed there */
#define PASSED_ON(arguments...) arguments

/* At 60 frames a second, a frame of the picture alone between two of the game's (between.c) */
#define FRAME_BETWEEN Host_FrameBetween()
#define FRAME_BETWEEN_SHOWN() Host_FrameBetweenShown()
#define FRAME_ENDED() Host_FrameEnded()
int Host_FrameBetween(void);
void Host_FrameBetweenShown(void);
void Host_FrameEnded(void);

/* Where a game frame ends: the menus patched (menus.c), the game clock's step (game_clock.c) */
#define FRAME_ENDING() Host_FrameEnding()
void Host_FrameEnding(void);

/* Operation 73's native code (native.c) */
#define GOOL_RUN_NATIVE Host_RunNative
struct GoolObject;
void Host_RunNative(struct GoolObject *object, int *state_pool, int **pc, int **sp, unsigned char *frame);

/* The GTE model's projections: narrowed across for the wide view, and kept before their rounding (precise.c) */
#define GTE_HORIZONTAL(offset) Host_Horizontal(offset)
long long Host_Horizontal(long long offset);
#define GTE_PROJECTED(sxy, view, h, ofx, ofy) Host_GteProjected((sxy), (const long long *)(view), (h), (ofx), (ofy))
void Host_GteProjected(unsigned int sxy, const long long *view, unsigned int h, int ofx, int ofy);

/* A path where the PlayStation's code reads what its callers left, which the C has no value for (host_log.c) */
#define CALLER_LEFT(function) Host_CallerLeft(#function)
void Host_CallerLeft(const char *function);
/* On such a path, a value for the variable the C leaves unset, and a return where the PlayStation's code returns what
 * its $v0 holds: decomp's own, so an optimizing compiler has one to keep */
#define CALLER_LEFT_VALUE(variable, value) ((variable) = (value))
#define LEFT_RETURN(value) return (value)

/* The operations of the host's patches of the game's GOOL code (menus.c) */
#define GOOL_HOST_OPERATION Host_GoolOperation
void Host_GoolOperation(struct GoolObject *object, unsigned int word);

#endif /* HOST_H */
