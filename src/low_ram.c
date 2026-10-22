#ifdef _WIN32
#include <windows.h>
#else
#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#endif
#include <stdint.h>
#include <string.h>

#include "host_log.h"
#include "low_ram.h"

/* The PlayStation reads and writes its first 64 KB, the kernel's RAM, where the game's null pointers and their fields
 * land (Gool_ResolveRef's null operand, read by Gool_OpVector). Linux maps no page there, so an access faults: the
 * handler finds the faulting instruction's base (or index) register, points it at s_LowRam by as much, runs that one
 * instruction (the trap flag), and puts the register back unless the instruction loaded into it. Linux reports the
 * fault and the step by signals, Windows by exceptions, to a vectored handler.
 * TRADEOFF: s_LowRam starts zeroed where the PlayStation holds what its BIOS left; an access with no register in its
 * address, or above 64 KB, still stops the program */

#define LOW_RAM_BYTES 0x10000
#define TRAP_FLAG 0x100
#define LOGGED_SITES 64

/* x86 instruction bytes */
#define PREFIX_OPERAND_SIZE 0x66
#define PREFIX_ADDRESS_SIZE 0x67
#define PREFIX_LOCK 0xF0
#define PREFIX_REPNE 0xF2
#define PREFIX_REP 0xF3
#define TWO_BYTE_ESCAPE 0x0F
#define PREFIX_SEGMENT_ES 0x26
#define PREFIX_SEGMENT_CS 0x2E
#define PREFIX_SEGMENT_SS 0x36
#define PREFIX_SEGMENT_DS 0x3E
#define PREFIX_SEGMENT_FS 0x64
#define PREFIX_SEGMENT_GS 0x65
/* The opcodes the handler tells apart: one-byte, then after the 0x0F escape */
#define OP_ADD_TO_REG8 0x02
#define OP_ADD_TO_REG 0x03
#define OP_SUB_TO_REG8 0x2A
#define OP_SUB_TO_REG 0x2B
#define OP_ALU_LAST 0x3F
#define OP_ALU_FORM_MASK 7
#define OP_ALU_TO_REG_LAST 3
#define OP_TEST_FIRST 0x84
#define OP_MOV_TO_REG8 0x8A
#define OP_MOV_TO_REG 0x8B
#define OP2_CMOV_FIRST 0x40
#define OP2_CMOV_LAST 0x4F
#define OP2_IMUL 0xAF
#define OP2_MOVZX8 0xB6
#define OP2_MOVZX16 0xB7
#define OP2_MOVSX8 0xBE
#define OP2_MOVSX16 0xBF
/* The ModRM byte's place after a one-byte opcode, and after the escape and the opcode */
#define MODRM_ONE_BYTE 1
#define MODRM_TWO_BYTE 2
#define MODRM_MOD(byte) ((byte) >> 6)
#define MODRM_REG(byte) (((byte) >> 3) & 7)
#define MODRM_RM(byte) ((byte) & 7)
#define MOD_INDIRECT 0
#define MOD_REGISTER 3
#define RM_SIB 4
#define RM_DISP32 5
#define SIB_SCALE(byte) ((byte) >> 6)
#define SIB_INDEX(byte) (((byte) >> 3) & 7)
#define SIB_BASE(byte) ((byte) & 7)
#define SIB_NO_INDEX 4
#define SIB_NO_BASE 5
#define NO_REGISTER -1

#define GENERAL_REGISTERS 8

static unsigned char s_LowRam[LOW_RAM_BYTES] __attribute__((aligned(16)));

/* The faulting thread's state as either system hands it over: the instruction's address, the general registers by
 * ModRM's numbers, the flags */
typedef struct {
    uint32_t eip;
    uint32_t *registers[GENERAL_REGISTERS];
    uint32_t *flags;
} Cpu;

/* The instruction being stepped: the register moved, by how much, and the one it writes */
static int s_Moved = NO_REGISTER;
static uint32_t s_Delta;
static int s_Written = NO_REGISTER;
static uint32_t s_Logged[LOGGED_SITES];
static int s_LoggedCount;

static int is_prefix(unsigned char byte) {
    switch (byte) {
    case PREFIX_OPERAND_SIZE:
    case PREFIX_LOCK:
    case PREFIX_REPNE:
    case PREFIX_REP:
    case PREFIX_SEGMENT_ES:
    case PREFIX_SEGMENT_CS:
    case PREFIX_SEGMENT_SS:
    case PREFIX_SEGMENT_DS:
    case PREFIX_SEGMENT_FS:
    case PREFIX_SEGMENT_GS:
        return 1;
    default:
        return 0;
    }
}

/* Whether the ModRM's reg field names a register operand (not an opcode's extension) */
static int uses_reg_field(unsigned char opcode, int two_byte) {
    if (two_byte) {
        return opcode == OP2_MOVZX8 || opcode == OP2_MOVZX16 || opcode == OP2_MOVSX8 || opcode == OP2_MOVSX16 ||
               opcode == OP2_IMUL || (opcode >= OP2_CMOV_FIRST && opcode <= OP2_CMOV_LAST);
    } else if (opcode <= OP_ALU_LAST) {
        return (opcode & OP_ALU_FORM_MASK) <= OP_ALU_TO_REG_LAST;
    } else {
        return opcode >= OP_TEST_FIRST && opcode <= OP_MOV_TO_REG;
    }
}

/* mov, movzx and movsx from memory: the register takes the loaded value whatever it held */
static int loads_register(unsigned char opcode, int two_byte) {
    if (two_byte) {
        return opcode == OP2_MOVZX8 || opcode == OP2_MOVZX16 || opcode == OP2_MOVSX8 || opcode == OP2_MOVSX16;
    } else {
        return opcode == OP_MOV_TO_REG8 || opcode == OP_MOV_TO_REG;
    }
}

/* add and sub into a register: one moved by s_Delta comes out moved by as much */
static int adds_into_register(unsigned char opcode, int two_byte) {
    return !two_byte && (opcode == OP_ADD_TO_REG8 || opcode == OP_ADD_TO_REG || opcode == OP_SUB_TO_REG8 ||
                         opcode == OP_SUB_TO_REG);
}

static void log_site(uint32_t eip, uint32_t address) {
    int index;

    for (index = 0; index < s_LoggedCount; index++) {
        if (s_Logged[index] == eip) {
            return;
        } else {
            /* Another site */
        }
    }
    if (s_LoggedCount < LOGGED_SITES) {
        s_Logged[s_LoggedCount++] = eip;
        Host_Log("low RAM: 0x%08X reads or writes 0x%04X", eip, address);
    } else {
        /* Enough sites logged */
    }
}

/* A fault on low RAM: the instruction's address register moved onto s_LowRam, the trap flag set */
static void on_fault(Cpu *cpu, uint32_t address) {
    const unsigned char *code = (const unsigned char *)(uintptr_t)cpu->eip;
    int two_byte = 0;
    unsigned char opcode;
    unsigned char modrm;
    int base = NO_REGISTER;
    int index = NO_REGISTER;
    int scale = 0;

    if (address >= LOW_RAM_BYTES || s_Moved != NO_REGISTER) {
        Host_Fatal("segmentation fault at 0x%08X, instruction 0x%08X", address, cpu->eip);
    } else {
        while (is_prefix(*code)) {
            code++;
        }
    }
    if (*code == PREFIX_ADDRESS_SIZE) {
        Host_Fatal("low RAM: 16-bit addressing at 0x%08X", cpu->eip);
    } else if (*code == TWO_BYTE_ESCAPE) {
        two_byte = 1;
        opcode = code[1];
        modrm = code[MODRM_TWO_BYTE];
        code += MODRM_TWO_BYTE + 1;
    } else {
        opcode = code[0];
        modrm = code[MODRM_ONE_BYTE];
        code += MODRM_ONE_BYTE + 1;
    }
    if (MODRM_MOD(modrm) == MOD_REGISTER || (MODRM_MOD(modrm) == MOD_INDIRECT && MODRM_RM(modrm) == RM_DISP32)) {
        Host_Fatal("low RAM: no register in the address of 0x%08X's access to 0x%04X", cpu->eip,
                   address);
    } else if (MODRM_RM(modrm) == RM_SIB) {
        unsigned char sib = *code;

        if (!(SIB_BASE(sib) == SIB_NO_BASE && MODRM_MOD(modrm) == MOD_INDIRECT)) {
            base = SIB_BASE(sib);
        } else {
            /* disp32 in place of a base */
        }
        if (SIB_INDEX(sib) != SIB_NO_INDEX) {
            index = SIB_INDEX(sib);
            scale = SIB_SCALE(sib);
        } else {
            /* No index */
        }
    } else {
        base = MODRM_RM(modrm);
    }
    if (base != NO_REGISTER) {
        s_Moved = base;
    } else if (index != NO_REGISTER && scale == 0) {
        s_Moved = index;
    } else {
        Host_Fatal("low RAM: no register to move in 0x%08X's address", cpu->eip);
    }
    log_site(cpu->eip, address);
    s_Written = NO_REGISTER;
    if (!uses_reg_field(opcode, two_byte) || MODRM_REG(modrm) != s_Moved) {
        /* The moved register is only the address's */
    } else if (loads_register(opcode, two_byte)) {
        s_Written = s_Moved;
    } else if (adds_into_register(opcode, two_byte)) {
        /* Put back after the step as an address register would be */
    } else {
        Host_Fatal("low RAM: 0x%08X uses its address register as an operand too", cpu->eip);
    }
    s_Delta = (uint32_t)(uintptr_t)s_LowRam;
    *cpu->registers[s_Moved] += s_Delta;
    *cpu->flags |= TRAP_FLAG;
}

/* After the stepped instruction: the moved register back, unless the instruction loaded into it */
static void on_step(Cpu *cpu) {
    if (s_Moved == NO_REGISTER) {
        Host_Fatal("trap at 0x%08X with no low RAM access stepping", cpu->eip);
    } else if (s_Written != s_Moved) {
        *cpu->registers[s_Moved] -= s_Delta;
    } else {
        /* The instruction loaded the register: its value stands */
    }
    *cpu->flags &= ~TRAP_FLAG;
    s_Moved = NO_REGISTER;
}

#ifdef _WIN32
/* AddVectoredExceptionHandler's order: first of the handlers */
#define HANDLER_CALLED_FIRST 1

/* A fault outside low RAM, or a step not this handler's, goes on to the system's handlers */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *pointers) {
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    CONTEXT *context = pointers->ContextRecord;
    Cpu cpu = {(uint32_t)context->Eip,
               {(uint32_t *)&context->Eax, (uint32_t *)&context->Ecx, (uint32_t *)&context->Edx,
                (uint32_t *)&context->Ebx, (uint32_t *)&context->Esp, (uint32_t *)&context->Ebp,
                (uint32_t *)&context->Esi, (uint32_t *)&context->Edi},
               (uint32_t *)&context->EFlags};

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->ExceptionInformation[1] < LOW_RAM_BYTES) {
        Host_InHandler(1);
        on_fault(&cpu, (uint32_t)record->ExceptionInformation[1]);
        Host_InHandler(0);
        return EXCEPTION_CONTINUE_EXECUTION;
    } else if (record->ExceptionCode == EXCEPTION_SINGLE_STEP && s_Moved != NO_REGISTER) {
        Host_InHandler(1);
        on_step(&cpu);
        Host_InHandler(0);
        return EXCEPTION_CONTINUE_EXECUTION;
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void Host_InstallLowRam(void) {
    if (AddVectoredExceptionHandler(HANDLER_CALLED_FIRST, on_exception) == NULL) {
        Host_Fatal("can't install the low RAM's exception handler");
    } else {
        /* Installed first in the handlers' chain */
    }
}
#else
static Cpu cpu_of(void *context_pointer) {
    greg_t *gregs = ((ucontext_t *)context_pointer)->uc_mcontext.gregs;
    Cpu cpu = {(uint32_t)gregs[REG_EIP],
               {(uint32_t *)&gregs[REG_EAX], (uint32_t *)&gregs[REG_ECX], (uint32_t *)&gregs[REG_EDX],
                (uint32_t *)&gregs[REG_EBX], (uint32_t *)&gregs[REG_ESP], (uint32_t *)&gregs[REG_EBP],
                (uint32_t *)&gregs[REG_ESI], (uint32_t *)&gregs[REG_EDI]},
               (uint32_t *)&gregs[REG_EFL]};

    return cpu;
}

static void on_segmentation_fault(int signal_number, siginfo_t *info, void *context_pointer) {
    Cpu cpu = cpu_of(context_pointer);

    (void)signal_number;
    Host_InHandler(1);
    on_fault(&cpu, (uint32_t)(uintptr_t)info->si_addr);
    Host_InHandler(0);
}

static void on_trap(int signal_number, siginfo_t *info, void *context_pointer) {
    Cpu cpu = cpu_of(context_pointer);

    (void)signal_number;
    (void)info;
    Host_InHandler(1);
    on_step(&cpu);
    Host_InHandler(0);
}

void Host_InstallLowRam(void) {
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = on_segmentation_fault;
    sigaction(SIGSEGV, &action, NULL);
    action.sa_sigaction = on_trap;
    sigaction(SIGTRAP, &action, NULL);
}
#endif
