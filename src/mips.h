#ifndef MIPS_H
#define MIPS_H

#include <stdint.h>

/* An interpreter of the R3000's integer instructions (mips.c), for the PlayStation's code the port runs as it is: the
 * GOOL entries' native blocks (native.c) and the retail executable's sound libraries (sdk.c). Like the R3000, a branch
 * runs its delay slot, a load's register takes its value one instruction late, and a division by 0 leaves the CPU's
 * results.
 *
 * Its memory: the retail executable's read-only data and code, from the player's disc (Mips_SetImage); the retail
 * data and bss, at the port's copy of them (retail.h); the scratchpad; the hardware's registers, through Mips_SetIo;
 * any other address the port's own, in the game's memory (low RAM, or the 8 MB window from RAM_WINDOW) or on the host's
 * stack */

enum {
    MIPS_REG_ZERO = 0,
    MIPS_REG_V0 = 2,
    MIPS_REG_A0 = 4,
    MIPS_REG_A1 = 5,
    MIPS_REG_A2 = 6,
    MIPS_REG_A3 = 7,
    MIPS_REG_SP = 29,
    MIPS_REG_RA = 31,
    MIPS_REGISTERS = 32
};

/* $ra on entry, which no code address equals: a jump there ends the run */
#define MIPS_RETURN_ADDRESS 0xFFFFFFF0u

/* What a run reports to its tracer (native.c's trace file): an instruction fetched, a word or halfword loaded or
 * stored */
enum {
    MIPS_TRACE_FETCH = 1,
    MIPS_TRACE_LOAD_WORD = 2,
    MIPS_TRACE_LOAD_HALF = 3,
    MIPS_TRACE_STORE_WORD = 4,
    MIPS_TRACE_STORE_HALF = 5
};

typedef struct MipsCpu {
    uint32_t regs[MIPS_REGISTERS];
    uint32_t hi, lo;
    uint32_t pc, next;
    /* A load's register and value, written after the instruction that follows it */
    int delayed_reg;
    uint32_t delayed_value;
    /* The run's tracer, or NULL */
    void (*trace)(uint32_t kind, uint32_t address, uint32_t value);
} MipsCpu;

/* A host function standing for the code at an address: it reads its arguments from the CPU (Mips_Argument), sets $v0
 * for a result, and the run goes on at $ra */
typedef void (*MipsHook)(MipsCpu *cpu);

/* The hardware's registers (0x1F801000 on, in any segment), by their physical address; bytes is 1, 2 or 4 */
typedef struct {
    uint32_t (*read)(uint32_t address, int bytes);
    void (*write)(uint32_t address, uint32_t value, int bytes);
} MipsIo;

void Mips_SetImage(const uint8_t *image, uint32_t bytes);
void Mips_SetIo(const MipsIo *io);
void Mips_AddHook(uint32_t address, MipsHook hook);

/* An interrupt for the host to deliver as soon as the code runs on: deliver runs between two instructions (on its own
 * CPU, so the interrupted code's state stays as it is), or when the outermost call returns. deliver may set itself
 * again, to wait (for interrupts to be enabled) */
void Mips_SetPending(void (*deliver)(void));

/* Runs from cpu->pc until a jump to MIPS_RETURN_ADDRESS, at most max_steps instructions */
void Mips_Run(MipsCpu *cpu, long max_steps);

/* Calls the code at address with up to 4 arguments on a stack of the interpreter's; returns $v0 */
uint32_t Mips_Call(uint32_t address, const uint32_t *arguments, int count);

/* A hook's argument by its index: $a0 to $a3, then the caller's stack */
uint32_t Mips_Argument(const MipsCpu *cpu, int index);

/* The interpreter's memory, for the host (a DMA's source) */
uint32_t Mips_Read(uint32_t address, int bytes);
void Mips_Write(uint32_t address, uint32_t value, int bytes);

#endif /* MIPS_H */
