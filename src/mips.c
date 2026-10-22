#include <stdint.h>
#include <string.h>

#include "host.h"
#include "host_log.h"
#include "mips.h"
#include "retail.h"

/* The R3000's integer instructions (mips.h). An instruction outside them (the coprocessors', syscall, a trapping
 * add's overflow) stops the program: none of the code run needs them */

#define INSTRUCTION_BYTES 4
/* jal's and jalr's link: the instruction after the delay slot */
#define LINK_OFFSET (2 * INSTRUCTION_BYTES)
#define WORD_MIN ((int32_t)0x80000000)
#define HIGH_SHIFT 32
#define LUI_SHIFT 16
#define SHIFT_MASK 31
#define BRANCH_SHIFT 2
#define JUMP_TARGET_MASK 0x03FFFFFFu
#define JUMP_REGION_MASK 0xF0000000u
#define BYTE_BITS 8
#define WORD_BYTES 4
#define HALF_BYTES 2
#define BYTE_MASK 0xFFu
#define HALF_MASK 0xFFFFu
#define ALIGN_MASK 3u

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
    OP_J = 2,
    OP_JAL = 3,
    OP_BEQ = 4,
    OP_BNE = 5,
    OP_BLEZ = 6,
    OP_BGTZ = 7,
    OP_ADDI = 8,
    OP_ADDIU = 9,
    OP_SLTI = 10,
    OP_SLTIU = 11,
    OP_ANDI = 12,
    OP_ORI = 13,
    OP_XORI = 14,
    OP_LUI = 15,
    OP_LB = 32,
    OP_LH = 33,
    OP_LWL = 34,
    OP_LW = 35,
    OP_LBU = 36,
    OP_LHU = 37,
    OP_LWR = 38,
    OP_SB = 40,
    OP_SH = 41,
    OP_SWL = 42,
    OP_SW = 43,
    OP_SWR = 46
};

enum {
    FUNCT_SLL = 0,
    FUNCT_SRL = 2,
    FUNCT_SRA = 3,
    FUNCT_SLLV = 4,
    FUNCT_SRLV = 6,
    FUNCT_SRAV = 7,
    FUNCT_JR = 8,
    FUNCT_JALR = 9,
    FUNCT_MFHI = 16,
    FUNCT_MTHI = 17,
    FUNCT_MFLO = 18,
    FUNCT_MTLO = 19,
    FUNCT_MULT = 24,
    FUNCT_MULTU = 25,
    FUNCT_DIV = 26,
    FUNCT_DIVU = 27,
    FUNCT_ADD = 32,
    FUNCT_ADDU = 33,
    FUNCT_SUB = 34,
    FUNCT_SUBU = 35,
    FUNCT_AND = 36,
    FUNCT_OR = 37,
    FUNCT_XOR = 38,
    FUNCT_NOR = 39,
    FUNCT_SLT = 42,
    FUNCT_SLTU = 43
};

/* REGIMM's branches: bit 0 of rt for "greater or equal", bit 4 for "and link" */
#define REGIMM_GREATER_EQUAL 1
#define REGIMM_LINK 0x10

/* The memory's regions */
#define LOW_RAM_END 0x10000u
#define PHYSICAL_MASK 0x1FFFFFFFu
#define SCRATCHPAD_START 0x1F800000u
#define SCRATCHPAD_BYTES 0x400u
#define IO_START 0x1F801000u
#define IO_END 0x1F803000u
/* The host's stack above the interpreter's frame, which holds the host's callers' locals the game passes the libraries
 * pointers to: Linux's default stack limit */
#define HOST_STACK_SPAN 0x800000u

/* The interpreter's stack, for Mips_Call; a call from a hook takes its stack this far below its caller's */
#define STACK_WORDS 0x4000
#define NESTED_STACK_GAP 0x100
/* The calling convention's room for the 4 arguments in registers, on the caller's stack */
#define ARGUMENT_REGISTERS 4
#define ARGUMENT_HOME_BYTES (ARGUMENT_REGISTERS * WORD_BYTES)
/* TRADEOFF: a call runs at most this many instructions, a guard against an interpreter bug looping forever */
#define CALL_MAX_STEPS 50000000L
#define HOOKS_MAX 32

static const uint8_t *s_Image;
static uint32_t s_ImageEnd;
static const MipsIo *s_Io;
static uint32_t s_HookAddress[HOOKS_MAX];
static MipsHook s_Hook[HOOKS_MAX];
static int s_Hooks;
static uint32_t s_Stack[STACK_WORDS];
static MipsCpu *s_Active;
static void (*s_Pending)(void);

/* The port's own code, whose function addresses its copy of the data holds where the retail data held the retail
 * code's (CMakeLists.txt links the program from __executable_start) */
extern char __executable_start[];
extern char __etext[];

void Mips_SetImage(const uint8_t *image, uint32_t bytes) {
    s_Image = image;
    s_ImageEnd = RETAIL_CODE_START + bytes;
}

void Mips_SetIo(const MipsIo *io) {
    s_Io = io;
}

void Mips_SetPending(void (*deliver)(void)) {
    s_Pending = deliver;
}

/* The pending interrupt delivered, once */
static void deliver_pending(void) {
    void (*deliver)(void) = s_Pending;

    s_Pending = NULL;
    deliver();
}

void Mips_AddHook(uint32_t address, MipsHook hook) {
    if (s_Hooks >= HOOKS_MAX) {
        Host_Fatal("more than %d hooks on the interpreter", HOOKS_MAX);
    } else {
        s_HookAddress[s_Hooks] = address;
        s_Hook[s_Hooks] = hook;
        s_Hooks++;
    }
}

static MipsHook hook_at(uint32_t address) {
    int index;

    for (index = 0; index < s_Hooks; index++) {
        if (s_HookAddress[index] == address) {
            return s_Hook[index];
        } else {
            continue;
        }
    }
    return NULL;
}

static int in_image(uint32_t address) {
    return s_Image != NULL && address >= RETAIL_CODE_START && address < RETAIL_DATA_START;
}

static int is_io(uint32_t address) {
    uint32_t physical = address & PHYSICAL_MASK;

    return address >= SCRATCHPAD_START && physical >= IO_START && physical < IO_END;
}

/* The host's bytes behind an address, or NULL for the hardware's registers */
static uint8_t *host_bytes(uint32_t address, int write) {
    uint32_t physical = address & PHYSICAL_MASK;
    uint32_t frame = (uint32_t)(uintptr_t)&physical;

    if (in_image(address) && write) {
        Host_Fatal("the interpreter's code writes the retail executable's code or read-only data at 0x%08X", address);
        return NULL;
    } else if (in_image(address)) {
        return (uint8_t *)s_Image + (address - RETAIL_CODE_START);
    } else if (s_Image != NULL && address >= RETAIL_DATA_START && address < RETAIL_BSS_END) {
        return (uint8_t *)g_EidAlphabet + (address - RETAIL_DATA_START);
    } else if (address >= SCRATCHPAD_START && physical >= SCRATCHPAD_START &&
               physical < SCRATCHPAD_START + SCRATCHPAD_BYTES) {
        return g_HostScratchpad + (physical - SCRATCHPAD_START);
    } else if (is_io(address)) {
        return NULL;
    } else if (address < LOW_RAM_END || (address >= RAM_WINDOW && address < RAM_WINDOW + RAM_WINDOW_BYTES) ||
               (address > frame && address - frame < HOST_STACK_SPAN)) {
        return (uint8_t *)(uintptr_t)address;
    } else {
        Host_Fatal("the interpreter's code at 0x%08X reaches 0x%08X, outside the game's memory",
                   s_Active == NULL ? 0 : s_Active->pc, address);
        return NULL;
    }
}

/* A word of the retail data as the retail code expects it: where the port's copy holds one of the port's functions,
 * the retail executable's word, the retail code's address of it */
static uint32_t retail_word(uint32_t address, uint32_t value) {
    uint32_t original;

    if (s_Image != NULL && address >= RETAIL_DATA_START && address + WORD_BYTES <= s_ImageEnd &&
        value >= (uint32_t)(uintptr_t)__executable_start && value < (uint32_t)(uintptr_t)__etext) {
        memcpy(&original, s_Image + (address - RETAIL_CODE_START), WORD_BYTES);
        return original >= RETAIL_CODE_START && original < RETAIL_DATA_START ? original : value;
    } else {
        return value;
    }
}

uint32_t Mips_Read(uint32_t address, int bytes) {
    uint8_t *host = host_bytes(address, 0);
    uint32_t value = 0;

    if (host == NULL && s_Io == NULL) {
        Host_Fatal("the interpreter's code reads the hardware at 0x%08X, with nothing behind it", address);
        return 0;
    } else if (host == NULL) {
        return s_Io->read(address & PHYSICAL_MASK, bytes);
    } else if (bytes == WORD_BYTES) {
        memcpy(&value, host, WORD_BYTES);
        return retail_word(address, value);
    } else {
        memcpy(&value, host, (size_t)bytes);
        return value;
    }
}

void Mips_Write(uint32_t address, uint32_t value, int bytes) {
    uint8_t *host = host_bytes(address, 1);

    if (host == NULL && s_Io == NULL) {
        Host_Fatal("the interpreter's code writes the hardware at 0x%08X, with nothing behind it", address);
    } else if (host == NULL) {
        s_Io->write(address & PHYSICAL_MASK, value, bytes);
    } else {
        memcpy(host, &value, (size_t)bytes);
    }
}

static uint32_t fetch(const MipsCpu *cpu) {
    uint32_t word;

    if (in_image(cpu->pc)) {
        memcpy(&word, s_Image + (cpu->pc - RETAIL_CODE_START), WORD_BYTES);
    } else {
        word = *(const uint32_t *)(uintptr_t)cpu->pc;
    }
    if (cpu->trace != NULL) {
        cpu->trace(MIPS_TRACE_FETCH, cpu->pc, word);
    } else {
        /* Not traced */
    }
    return word;
}

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

static void fail(const MipsCpu *cpu, uint32_t word) {
    Host_Fatal("the interpreter doesn't know instruction 0x%08X at 0x%08X", word, cpu->pc);
}

/* SPECIAL's operations: the register written, or MIPS_REG_ZERO for none */
static int special(MipsCpu *cpu, uint32_t word, uint32_t *next) {
    uint32_t s = cpu->regs[RS(word)];
    uint32_t t = cpu->regs[RT(word)];
    int d = RD(word);
    int64_t product;
    uint64_t unsigned_product;

    switch (FUNCT(word)) {
    case FUNCT_SLL:
        cpu->regs[d] = t << SHAMT(word);
        return d;
    case FUNCT_SRL:
        cpu->regs[d] = t >> SHAMT(word);
        return d;
    case FUNCT_SRA:
        cpu->regs[d] = (uint32_t)((int32_t)t >> SHAMT(word));
        return d;
    case FUNCT_SLLV:
        cpu->regs[d] = t << (s & SHIFT_MASK);
        return d;
    case FUNCT_SRLV:
        cpu->regs[d] = t >> (s & SHIFT_MASK);
        return d;
    case FUNCT_SRAV:
        cpu->regs[d] = (uint32_t)((int32_t)t >> (s & SHIFT_MASK));
        return d;
    case FUNCT_JR:
        *next = s;
        return MIPS_REG_ZERO;
    case FUNCT_JALR:
        *next = s;
        cpu->regs[d] = cpu->pc + LINK_OFFSET;
        return d;
    case FUNCT_MFHI:
        cpu->regs[d] = cpu->hi;
        return d;
    case FUNCT_MTHI:
        cpu->hi = s;
        return MIPS_REG_ZERO;
    case FUNCT_MFLO:
        cpu->regs[d] = cpu->lo;
        return d;
    case FUNCT_MTLO:
        cpu->lo = s;
        return MIPS_REG_ZERO;
    case FUNCT_MULT:
        product = (int64_t)(int32_t)s * (int32_t)t;
        cpu->lo = (uint32_t)product;
        cpu->hi = (uint32_t)((uint64_t)product >> HIGH_SHIFT);
        return MIPS_REG_ZERO;
    case FUNCT_MULTU:
        unsigned_product = (uint64_t)s * t;
        cpu->lo = (uint32_t)unsigned_product;
        cpu->hi = (uint32_t)(unsigned_product >> HIGH_SHIFT);
        return MIPS_REG_ZERO;
    case FUNCT_DIV:
        cpu->lo = (uint32_t)quotient((int32_t)s, (int32_t)t);
        cpu->hi = (uint32_t)remainder_of((int32_t)s, (int32_t)t);
        return MIPS_REG_ZERO;
    case FUNCT_DIVU:
        /* A division by 0 leaves all ones and the dividend */
        cpu->lo = t == 0 ? UINT32_MAX : s / t;
        cpu->hi = t == 0 ? s : s % t;
        return MIPS_REG_ZERO;
    case FUNCT_ADD:
        /* TRADEOFF: add's and sub's overflow traps left out; the PlayStation would stop on one, so the code run
         * never overflows */
    case FUNCT_ADDU:
        cpu->regs[d] = s + t;
        return d;
    case FUNCT_SUB:
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
    case FUNCT_SLTU:
        cpu->regs[d] = s < t;
        return d;
    default:
        fail(cpu, word);
        return MIPS_REG_ZERO;
    }
}

static void branch(int taken, uint32_t word, uint32_t delay_slot, uint32_t *next) {
    if (taken) {
        *next = delay_slot + (IMMEDIATE(word) << BRANCH_SHIFT);
    } else {
        /* Not taken */
    }
}

/* The register a load is about to write as the R3000 sees it: a load still pending into it forwards its value (lwl
 * and lwr merge into the word the load before them brought) */
static uint32_t pending(const MipsCpu *cpu, int reg) {
    return cpu->delayed_reg == reg ? cpu->delayed_value : cpu->regs[reg];
}

/* lwl and lwr: the aligned word's bytes merged into the register's, from the left (its high bytes) or the right */
static uint32_t load_left(uint32_t address, uint32_t old) {
    uint32_t shift = (address & ALIGN_MASK) * BYTE_BITS;
    uint32_t word = Mips_Read(address & ~ALIGN_MASK, WORD_BYTES);
    uint32_t kept = shift == (ALIGN_MASK * BYTE_BITS) ? 0 : old & (UINT32_MAX >> (shift + BYTE_BITS));

    return (word << ((ALIGN_MASK * BYTE_BITS) - shift)) | kept;
}

static uint32_t load_right(uint32_t address, uint32_t old) {
    uint32_t shift = (address & ALIGN_MASK) * BYTE_BITS;
    uint32_t word = Mips_Read(address & ~ALIGN_MASK, WORD_BYTES);
    uint32_t kept = shift == 0 ? 0 : old & ~(UINT32_MAX >> shift);

    return (word >> shift) | kept;
}

/* swl and swr: the register's high bytes stored down to the address, or its low bytes up from it */
static void store_left(uint32_t address, uint32_t value) {
    uint32_t bytes = (address & ALIGN_MASK) + 1;
    uint32_t index;

    for (index = 0; index < bytes; index++) {
        Mips_Write((address & ~ALIGN_MASK) + index,
                   (value >> ((WORD_BYTES - bytes + index) * BYTE_BITS)) & BYTE_MASK, 1);
    }
}

static void store_right(uint32_t address, uint32_t value) {
    uint32_t first = address & ALIGN_MASK;
    uint32_t index;

    for (index = first; index < WORD_BYTES; index++) {
        Mips_Write((address & ~ALIGN_MASK) + index, (value >> ((index - first) * BYTE_BITS)) & BYTE_MASK, 1);
    }
}

static void trace(const MipsCpu *cpu, uint32_t kind, uint32_t address, uint32_t value) {
    if (cpu->trace != NULL) {
        cpu->trace(kind, address, value);
    } else {
        /* Not traced */
    }
}

/* A load's or store's instruction: the register a load writes, and its value, in *loaded and *load_value */
static void memory(MipsCpu *cpu, uint32_t word, int *loaded, uint32_t *load_value) {
    uint32_t t = cpu->regs[RT(word)];
    uint32_t address = cpu->regs[RS(word)] + IMMEDIATE(word);
    int r = RT(word);

    switch (OPCODE(word)) {
    case OP_LB:
        *loaded = r;
        *load_value = (uint32_t)(int32_t)(int8_t)Mips_Read(address, 1);
        return;
    case OP_LBU:
        *loaded = r;
        *load_value = Mips_Read(address, 1);
        return;
    case OP_LH:
        *loaded = r;
        *load_value = (uint32_t)(int32_t)(int16_t)Mips_Read(address, HALF_BYTES);
        trace(cpu, MIPS_TRACE_LOAD_HALF, address, *load_value & HALF_MASK);
        return;
    case OP_LHU:
        *loaded = r;
        *load_value = Mips_Read(address, HALF_BYTES);
        return;
    case OP_LW:
        *loaded = r;
        *load_value = Mips_Read(address, WORD_BYTES);
        trace(cpu, MIPS_TRACE_LOAD_WORD, address, *load_value);
        return;
    case OP_LWL:
        *loaded = r;
        *load_value = load_left(address, pending(cpu, r));
        return;
    case OP_LWR:
        *loaded = r;
        *load_value = load_right(address, pending(cpu, r));
        return;
    case OP_SB:
        Mips_Write(address, t & BYTE_MASK, 1);
        return;
    case OP_SH:
        Mips_Write(address, t & HALF_MASK, HALF_BYTES);
        trace(cpu, MIPS_TRACE_STORE_HALF, address, t & HALF_MASK);
        return;
    case OP_SW:
        Mips_Write(address, t, WORD_BYTES);
        trace(cpu, MIPS_TRACE_STORE_WORD, address, t);
        return;
    case OP_SWL:
        store_left(address, t);
        return;
    case OP_SWR:
        store_right(address, t);
        return;
    default:
        fail(cpu, word);
        return;
    }
}

/* One instruction: the register it wrote (MIPS_REG_ZERO for none), its load's in *loaded (MIPS_REG_ZERO for none) */
static int step(MipsCpu *cpu, uint32_t word, uint32_t *next, int *loaded, uint32_t *load_value) {
    uint32_t s = cpu->regs[RS(word)];
    uint32_t t = cpu->regs[RT(word)];
    uint32_t delay_slot = cpu->pc + INSTRUCTION_BYTES;
    int r = RT(word);

    *loaded = MIPS_REG_ZERO;
    switch (OPCODE(word)) {
    case OP_SPECIAL:
        return special(cpu, word, next);
    case OP_REGIMM:
        branch((RT(word) & REGIMM_GREATER_EQUAL) ? (int32_t)s >= 0 : (int32_t)s < 0, word, delay_slot, next);
        if (RT(word) & REGIMM_LINK) {
            cpu->regs[MIPS_REG_RA] = cpu->pc + LINK_OFFSET;
            return MIPS_REG_RA;
        } else {
            return MIPS_REG_ZERO;
        }
    case OP_J:
        *next = (delay_slot & JUMP_REGION_MASK) | ((word & JUMP_TARGET_MASK) << BRANCH_SHIFT);
        return MIPS_REG_ZERO;
    case OP_JAL:
        *next = (delay_slot & JUMP_REGION_MASK) | ((word & JUMP_TARGET_MASK) << BRANCH_SHIFT);
        cpu->regs[MIPS_REG_RA] = cpu->pc + LINK_OFFSET;
        return MIPS_REG_RA;
    case OP_BEQ:
        branch(s == t, word, delay_slot, next);
        return MIPS_REG_ZERO;
    case OP_BNE:
        branch(s != t, word, delay_slot, next);
        return MIPS_REG_ZERO;
    case OP_BLEZ:
        branch((int32_t)s <= 0, word, delay_slot, next);
        return MIPS_REG_ZERO;
    case OP_BGTZ:
        branch((int32_t)s > 0, word, delay_slot, next);
        return MIPS_REG_ZERO;
    case OP_ADDI:
        /* TRADEOFF: addi's overflow trap left out, as add's */
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
    case OP_LB:
    case OP_LH:
    case OP_LWL:
    case OP_LW:
    case OP_LBU:
    case OP_LHU:
    case OP_LWR:
    case OP_SB:
    case OP_SH:
    case OP_SWL:
    case OP_SW:
    case OP_SWR:
        memory(cpu, word, loaded, load_value);
        return MIPS_REG_ZERO;
    default:
        fail(cpu, word);
        return MIPS_REG_ZERO;
    }
}

/* The pending load lands, as it does before a hook's host code reads the registers */
static void land(MipsCpu *cpu) {
    if (cpu->delayed_reg != MIPS_REG_ZERO) {
        cpu->regs[cpu->delayed_reg] = cpu->delayed_value;
    } else {
        /* No load pending */
    }
    cpu->delayed_reg = MIPS_REG_ZERO;
}

void Mips_Run(MipsCpu *cpu, long max_steps) {
    MipsCpu *caller = s_Active;
    long steps;

    s_Active = cpu;
    for (steps = 0; cpu->pc != MIPS_RETURN_ADDRESS; steps++) {
        MipsHook hook = in_image(cpu->pc) ? hook_at(cpu->pc) : NULL;
        uint32_t word;
        uint32_t next = cpu->next + INSTRUCTION_BYTES;
        int loaded;
        uint32_t load_value;
        int written = MIPS_REG_ZERO;

        if (s_Pending != NULL) {
            deliver_pending();
        } else {
            /* No interrupt */
        }
        if (steps >= max_steps) {
            Host_Fatal("the interpreter ran %ld instructions without returning, at 0x%08X", max_steps, cpu->pc);
        } else if (hook != NULL) {
            land(cpu);
            hook(cpu);
            cpu->pc = cpu->regs[MIPS_REG_RA];
            cpu->next = cpu->pc + INSTRUCTION_BYTES;
            continue;
        } else {
            word = fetch(cpu);
            written = step(cpu, word, &next, &loaded, &load_value);
        }
        /* The previous load lands now, unless this instruction wrote its register */
        if (cpu->delayed_reg != MIPS_REG_ZERO && cpu->delayed_reg != written) {
            cpu->regs[cpu->delayed_reg] = cpu->delayed_value;
        } else {
            /* No load pending, or it was overwritten */
        }
        cpu->delayed_reg = loaded;
        cpu->delayed_value = load_value;
        cpu->regs[MIPS_REG_ZERO] = 0;
        cpu->pc = cpu->next;
        cpu->next = next;
    }
    land(cpu);
    s_Active = caller;
}

uint32_t Mips_Call(uint32_t address, const uint32_t *arguments, int count) {
    MipsCpu cpu;
    uint32_t stack_top;
    int index;

    if (count > ARGUMENT_REGISTERS) {
        Host_Fatal("a call into the interpreter with %d arguments, more than its %d registers", count,
                   ARGUMENT_REGISTERS);
    } else {
        memset(&cpu, 0, sizeof(cpu));
    }
    if (s_Active != NULL) {
        stack_top = s_Active->regs[MIPS_REG_SP] - NESTED_STACK_GAP;
    } else {
        stack_top = (uint32_t)(uintptr_t)&s_Stack[STACK_WORDS];
    }
    for (index = 0; index < count; index++) {
        cpu.regs[MIPS_REG_A0 + index] = arguments[index];
    }
    cpu.regs[MIPS_REG_SP] = stack_top - ARGUMENT_HOME_BYTES;
    cpu.regs[MIPS_REG_RA] = MIPS_RETURN_ADDRESS;
    cpu.pc = address;
    cpu.next = address + INSTRUCTION_BYTES;
    cpu.delayed_reg = MIPS_REG_ZERO;
    Mips_Run(&cpu, CALL_MAX_STEPS);
    if (s_Active == NULL && s_Pending != NULL) {
        deliver_pending();
    } else {
        /* Within another call, whose run delivers it, or none pending */
    }
    return cpu.regs[MIPS_REG_V0];
}

uint32_t Mips_Argument(const MipsCpu *cpu, int index) {
    if (index < ARGUMENT_REGISTERS) {
        return cpu->regs[MIPS_REG_A0 + index];
    } else {
        return Mips_Read(cpu->regs[MIPS_REG_SP] + (uint32_t)index * WORD_BYTES, WORD_BYTES);
    }
}
