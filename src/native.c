#include <stdint.h>

#include "host.h"
#include "host_log.h"

/* Operation 73's native code: MIPS from the disc's GOOL entries, run on an interpreter of the instructions the blocks
 * use (decomp/docs/TARGET.md, "The GOOL interpreter": 32 of the integer set besides nop, memory reached only through
 * registers). Like the R3000, a branch runs its delay slot, a load's register takes its value one instruction late,
 * and a division by 0 leaves the CPU's results. The block reads the registers decomp's gool_native sets and ends by a
 * jump to $ra: $s5 then holds the GOOL instruction to go on from, or 0 to return */

enum {
    REG_ZERO = 0,
    REG_S0 = 16,
    REG_S3 = 19,
    REG_S5 = 21,
    REG_S6 = 22,
    REG_S7 = 23,
    REG_FP = 30,
    REG_RA = 31,
    REGISTERS = 32
};

/* $ra on entry, which no code address equals: a jump there ends the block */
#define RETURN_ADDRESS 0xFFFFFFF0u
/* TRADEOFF: a block runs at most this many instructions, a guard against an interpreter bug looping forever */
#define MAX_STEPS 10000000
#define INSTRUCTION_BYTES 4
/* jalr's link: the instruction after its delay slot */
#define LINK_OFFSET (2 * INSTRUCTION_BYTES)
#define WORD_MIN ((int32_t)0x80000000)
#define HIGH_SHIFT 32
#define LUI_SHIFT 16
#define SHIFT_MASK 31
#define BRANCH_SHIFT 2

/* Fields of an instruction */
#define OPCODE(word) ((word) >> 26)
#define RS(word) (((word) >> 21) & 31)
#define RT(word) (((word) >> 16) & 31)
#define RD(word) (((word) >> 11) & 31)
#define SHAMT(word) (((word) >> 6) & 31)
#define FUNCT(word) ((word) & 63)
#define IMMEDIATE(word) ((uint32_t)(int32_t)(int16_t)((word) & 0xFFFF))
#define UNSIGNED_IMMEDIATE(word) ((word) & 0xFFFF)

enum {
    OP_SPECIAL = 0,
    OP_REGIMM = 1,
    OP_BEQ = 4,
    OP_BNE = 5,
    OP_ADDIU = 9,
    OP_SLTI = 10,
    OP_SLTIU = 11,
    OP_ANDI = 12,
    OP_ORI = 13,
    OP_XORI = 14,
    OP_LUI = 15,
    OP_LH = 33,
    OP_LW = 35,
    OP_SH = 41,
    OP_SW = 43
};

enum {
    FUNCT_SLL = 0,
    FUNCT_SRA = 3,
    FUNCT_SLLV = 4,
    FUNCT_SRAV = 7,
    FUNCT_JR = 8,
    FUNCT_JALR = 9,
    FUNCT_MFHI = 16,
    FUNCT_MFLO = 18,
    FUNCT_MULT = 24,
    FUNCT_DIV = 26,
    FUNCT_ADDU = 33,
    FUNCT_SUB = 34,
    FUNCT_SUBU = 35,
    FUNCT_AND = 36,
    FUNCT_OR = 37,
    FUNCT_XOR = 38,
    FUNCT_NOR = 39,
    FUNCT_SLT = 42
};

#define REGIMM_BGEZ 1

typedef struct {
    uint32_t regs[REGISTERS];
    uint32_t hi, lo;
    uint32_t pc, next;
    /* A load's register and value, written after the instruction that follows it */
    int delayed_reg;
    uint32_t delayed_value;
} Cpu;

static int32_t quotient(int32_t dividend, int32_t divisor) {
    if (divisor == 0) {
        if (dividend >= 0) {
            return -1;
        } else {
            return 1;
        }
    } else if (dividend == WORD_MIN && divisor == -1) {
        return WORD_MIN;
    } else {
        return dividend / divisor;
    }
}

static int32_t remainder_of(int32_t dividend, int32_t divisor) {
    if (divisor == 0) {
        return dividend;
    } else if (dividend == WORD_MIN && divisor == -1) {
        return 0;
    } else {
        return dividend % divisor;
    }
}

static void fail(const Cpu *cpu, uint32_t word) {
    Host_Fatal("native code: instruction 0x%08X at 0x%08X is not one the interpreter knows", word, cpu->pc);
}

/* SPECIAL's operations: the register written, or REG_ZERO for none */
static int special(Cpu *cpu, uint32_t word, uint32_t *next) {
    uint32_t s = cpu->regs[RS(word)];
    uint32_t t = cpu->regs[RT(word)];
    int d = RD(word);
    int64_t product;

    switch (FUNCT(word)) {
    case FUNCT_SLL:
        cpu->regs[d] = t << SHAMT(word);
        return d;
    case FUNCT_SRA:
        cpu->regs[d] = (uint32_t)((int32_t)t >> SHAMT(word));
        return d;
    case FUNCT_SLLV:
        cpu->regs[d] = t << (s & SHIFT_MASK);
        return d;
    case FUNCT_SRAV:
        cpu->regs[d] = (uint32_t)((int32_t)t >> (s & SHIFT_MASK));
        return d;
    case FUNCT_JR:
        *next = s;
        return REG_ZERO;
    case FUNCT_JALR:
        *next = s;
        cpu->regs[d] = cpu->pc + LINK_OFFSET;
        return d;
    case FUNCT_MFHI:
        cpu->regs[d] = cpu->hi;
        return d;
    case FUNCT_MFLO:
        cpu->regs[d] = cpu->lo;
        return d;
    case FUNCT_MULT:
        product = (int64_t)(int32_t)s * (int32_t)t;
        cpu->lo = (uint32_t)product;
        cpu->hi = (uint32_t)((uint64_t)product >> HIGH_SHIFT);
        return REG_ZERO;
    case FUNCT_DIV:
        cpu->lo = (uint32_t)quotient((int32_t)s, (int32_t)t);
        cpu->hi = (uint32_t)remainder_of((int32_t)s, (int32_t)t);
        return REG_ZERO;
    case FUNCT_ADDU:
        cpu->regs[d] = s + t;
        return d;
    case FUNCT_SUB:
        /* TRADEOFF: sub's overflow trap left out; the PlayStation would stop on it, so the blocks never overflow */
    case FUNCT_SUBU:
        cpu->regs[d] = s - t;
        return d;
    case FUNCT_AND:
        cpu->regs[d] = s & t;
        return d;
    case FUNCT_OR:
        cpu->regs[d] = s | t;
        return d;
    case FUNCT_XOR:
        cpu->regs[d] = s ^ t;
        return d;
    case FUNCT_NOR:
        cpu->regs[d] = ~(s | t);
        return d;
    case FUNCT_SLT:
        cpu->regs[d] = (int32_t)s < (int32_t)t;
        return d;
    default:
        fail(cpu, word);
        return REG_ZERO;
    }
}

/* One instruction: the register it wrote (REG_ZERO for none), its load's in *loaded (REG_ZERO for none) */
static int step(Cpu *cpu, uint32_t word, uint32_t *next, int *loaded, uint32_t *load_value) {
    uint32_t s = cpu->regs[RS(word)];
    uint32_t t = cpu->regs[RT(word)];
    uint32_t address = s + IMMEDIATE(word);
    uint32_t delay_slot = cpu->pc + INSTRUCTION_BYTES;
    int r = RT(word);

    *loaded = REG_ZERO;
    switch (OPCODE(word)) {
    case OP_SPECIAL:
        return special(cpu, word, next);
    case OP_REGIMM:
        if (RT(word) == REGIMM_BGEZ) {
            if ((int32_t)s >= 0) {
                *next = delay_slot + (IMMEDIATE(word) << BRANCH_SHIFT);
            } else {
                /* Not taken */
            }
            return REG_ZERO;
        } else {
            fail(cpu, word);
            return REG_ZERO;
        }
    case OP_BEQ:
        if (s == t) {
            *next = delay_slot + (IMMEDIATE(word) << BRANCH_SHIFT);
        } else {
            /* Not taken */
        }
        return REG_ZERO;
    case OP_BNE:
        if (s != t) {
            *next = delay_slot + (IMMEDIATE(word) << BRANCH_SHIFT);
        } else {
            /* Not taken */
        }
        return REG_ZERO;
    case OP_ADDIU:
        cpu->regs[r] = s + IMMEDIATE(word);
        return r;
    case OP_SLTI:
        cpu->regs[r] = (int32_t)s < (int32_t)IMMEDIATE(word);
        return r;
    case OP_SLTIU:
        cpu->regs[r] = s < IMMEDIATE(word);
        return r;
    case OP_ANDI:
        cpu->regs[r] = s & UNSIGNED_IMMEDIATE(word);
        return r;
    case OP_ORI:
        cpu->regs[r] = s | UNSIGNED_IMMEDIATE(word);
        return r;
    case OP_XORI:
        cpu->regs[r] = s ^ UNSIGNED_IMMEDIATE(word);
        return r;
    case OP_LUI:
        cpu->regs[r] = UNSIGNED_IMMEDIATE(word) << LUI_SHIFT;
        return r;
    case OP_LH:
        *loaded = r;
        *load_value = (uint32_t)(int32_t)*(int16_t *)(uintptr_t)address;
        return REG_ZERO;
    case OP_LW:
        *loaded = r;
        *load_value = *(uint32_t *)(uintptr_t)address;
        return REG_ZERO;
    case OP_SH:
        *(uint16_t *)(uintptr_t)address = (uint16_t)t;
        return REG_ZERO;
    case OP_SW:
        *(uint32_t *)(uintptr_t)address = t;
        return REG_ZERO;
    default:
        fail(cpu, word);
        return REG_ZERO;
    }
}

void Host_RunNative(struct GoolObject *object, int *state_pool, int **pc, int **sp, unsigned char *frame) {
    Cpu cpu = {0};
    long steps;

    cpu.regs[REG_S0] = (uint32_t)(uintptr_t)object;
    cpu.regs[REG_S3] = (uint32_t)(uintptr_t)state_pool;
    cpu.regs[REG_S5] = (uint32_t)(uintptr_t)*pc;
    cpu.regs[REG_S6] = (uint32_t)(uintptr_t)*sp;
    cpu.regs[REG_S7] = (uint32_t)(uintptr_t)frame;
    cpu.regs[REG_FP] = SCRATCHPAD;
    cpu.regs[REG_RA] = RETURN_ADDRESS;
    cpu.pc = cpu.regs[REG_S5];
    cpu.next = cpu.pc + INSTRUCTION_BYTES;
    cpu.delayed_reg = REG_ZERO;
    for (steps = 0; cpu.pc != RETURN_ADDRESS; steps++) {
        uint32_t word = *(uint32_t *)(uintptr_t)cpu.pc;
        uint32_t next = cpu.next + INSTRUCTION_BYTES;
        int loaded;
        uint32_t load_value;
        int written = REG_ZERO;

        if (steps >= MAX_STEPS) {
            Host_Fatal("native code from 0x%08X ran %d instructions without returning", (unsigned int)(uintptr_t)*pc,
                       MAX_STEPS);
        } else {
            written = step(&cpu, word, &next, &loaded, &load_value);
        }
        /* The previous load lands now, unless this instruction wrote its register */
        if (cpu.delayed_reg != REG_ZERO && cpu.delayed_reg != written) {
            cpu.regs[cpu.delayed_reg] = cpu.delayed_value;
        } else {
            /* No load pending, or it was overwritten */
        }
        cpu.delayed_reg = loaded;
        cpu.delayed_value = load_value;
        cpu.regs[REG_ZERO] = 0;
        cpu.pc = cpu.next;
        cpu.next = next;
    }
    if (cpu.delayed_reg != REG_ZERO) {
        cpu.regs[cpu.delayed_reg] = cpu.delayed_value;
    } else {
        /* No load pending */
    }
    *pc = (int *)(uintptr_t)cpu.regs[REG_S5];
    *sp = (int *)(uintptr_t)cpu.regs[REG_S6];
}
