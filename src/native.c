#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "host_log.h"
#include "mips.h"

/* Operation 73's native code: MIPS from the disc's GOOL entries, run on the interpreter (mips.h; decomp/docs/TARGET.md,
 * "The GOOL interpreter": memory reached only through registers). The block reads the registers decomp's gool_native
 * sets and ends by a jump to $ra: $s5 then holds the GOOL instruction to go on from, or 0 to return */

enum {
    REG_S0 = 16,
    REG_S3 = 19,
    REG_S5 = 21,
    REG_S6 = 22,
    REG_S7 = 23,
    REG_FP = 30
};

/* TRADEOFF: a block runs at most this many instructions, a guard against an interpreter bug looping forever */
#define MAX_STEPS 10000000
#define INSTRUCTION_BYTES 4

/* With CRASH2_NATIVE_TRACE naming a file, blocks' runs go there for tools/check_native.py to replay on an emulator:
 * each block's registers on entry, its fetches, loads and stores with their values, its registers at the end, as
 * little-endian words. Each block (by its start) is traced its first TRACED_RUNS times, up to TRACED_BLOCKS runs */
#define TRACE_VARIABLE "CRASH2_NATIVE_TRACE"
#define TRACED_BLOCKS 50000
#define TRACED_RUNS 16
#define TRACED_STARTS 4096
#define TRACED_START_MASK (TRACED_STARTS - 1)
/* Knuth's multiplicative hash: 2^32 over the golden ratio */
#define START_HASH 2654435761u
/* The records besides the interpreter's (MIPS_TRACE_FETCH to MIPS_TRACE_STORE_HALF): a block's start and end */
enum {
    TRACE_BEGIN = 0x4E494742,
    TRACE_END = 0x444E4542
};

static FILE *s_Trace;
static int s_TraceChecked;
static int s_TracedBlocks;
static int s_Tracing;
/* Each start's address and runs traced, by its hash; a full table traces nothing more */
static uint32_t s_TracedStart[TRACED_STARTS];
static int s_TracedRuns[TRACED_STARTS];

static int *runs_of(uint32_t start) {
    uint32_t index = (start * START_HASH) & TRACED_START_MASK;
    int probes;

    for (probes = 0; probes < TRACED_STARTS; probes++) {
        uint32_t slot = (index + (uint32_t)probes) & TRACED_START_MASK;

        if (s_TracedStart[slot] == start || s_TracedStart[slot] == 0) {
            s_TracedStart[slot] = start;
            return &s_TracedRuns[slot];
        } else {
            /* Another start */
        }
    }
    return NULL;
}

/* The traced block's instructions so far */
static uint32_t s_Steps;

static void trace(uint32_t kind, uint32_t address, uint32_t value) {
    uint32_t record[3];

    if (s_Tracing) {
        s_Steps += kind == MIPS_TRACE_FETCH;
        record[0] = kind;
        record[1] = address;
        record[2] = value;
        fwrite(record, sizeof(record), 1, s_Trace);
    } else {
        /* Not traced */
    }
}

static void trace_registers(const MipsCpu *cpu) {
    if (s_Tracing) {
        fwrite(cpu->regs, sizeof(cpu->regs), 1, s_Trace);
        /* Whole blocks on disk, should the run stop */
        fflush(s_Trace);
    } else {
        /* Not traced */
    }
}

/* Whether this block is traced: the file opened on the first block, closed after the last traced */
static void start_trace(uint32_t start) {
    const char *path;
    int *runs;

    if (!s_TraceChecked) {
        s_TraceChecked = 1;
        path = getenv(TRACE_VARIABLE);
        s_Trace = path == NULL ? NULL : fopen(path, "wb");
    } else {
        /* Opened or not already */
    }
    runs = s_Trace == NULL ? NULL : runs_of(start);
    if (s_Trace != NULL && s_TracedBlocks < TRACED_BLOCKS && runs != NULL && *runs >= TRACED_RUNS) {
        s_Tracing = 0;
    } else if (s_Trace != NULL && s_TracedBlocks < TRACED_BLOCKS && runs != NULL) {
        (*runs)++;
        s_TracedBlocks++;
        s_Tracing = 1;
    } else if (s_Trace != NULL) {
        fclose(s_Trace);
        s_Trace = NULL;
        s_Tracing = 0;
        Host_Log("native trace: %d blocks written", TRACED_BLOCKS);
    } else {
        s_Tracing = 0;
    }
}

void Host_RunNative(struct GoolObject *object, int *state_pool, int **pc, int **sp, unsigned char *frame) {
    MipsCpu cpu = {0};

    cpu.regs[REG_S0] = (uint32_t)(uintptr_t)object;
    cpu.regs[REG_S3] = (uint32_t)(uintptr_t)state_pool;
    cpu.regs[REG_S5] = (uint32_t)(uintptr_t)*pc;
    cpu.regs[REG_S6] = (uint32_t)(uintptr_t)*sp;
    cpu.regs[REG_S7] = (uint32_t)(uintptr_t)frame;
    cpu.regs[REG_FP] = SCRATCHPAD;
    cpu.regs[MIPS_REG_RA] = MIPS_RETURN_ADDRESS;
    cpu.pc = cpu.regs[REG_S5];
    cpu.next = cpu.pc + INSTRUCTION_BYTES;
    cpu.delayed_reg = MIPS_REG_ZERO;
    start_trace(cpu.pc);
    s_Steps = 0;
    cpu.trace = s_Tracing ? trace : NULL;
    trace(TRACE_BEGIN, cpu.pc, 0);
    trace_registers(&cpu);
    Mips_Run(&cpu, MAX_STEPS);
    trace(TRACE_END, s_Steps, 0);
    trace_registers(&cpu);
    *pc = (int *)(uintptr_t)cpu.regs[REG_S5];
    *sp = (int *)(uintptr_t)cpu.regs[REG_S6];
}
