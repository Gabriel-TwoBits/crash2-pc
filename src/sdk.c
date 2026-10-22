#include <stdint.h>
#include <string.h>

#include "host_log.h"
#include "mips.h"
#include "psyq/host_psyq.h"
#include "retail.h"
#include "sdk_symbols.h"
#include "spu_host.h"

/* The sound libraries, libsnd and libspu, as the retail executable has them: its code, from the player's disc, run on
 * the interpreter (mips.h), so the music and the sound effects are the PlayStation's own. The game's calls into them
 * go through the functions below; theirs to the rest of the system (libapi, libetc, libc) come to the hooks; their
 * reads and writes of the hardware reach the SPU (spu_host.h), the sound's DMA channel and the interrupt controller */

#define EXECUTABLE "SCUS_941.54;1"
/* A PS-X EXE's header: its identifier, and its text's address and size, which the rest of the file loads at */
#define EXE_HEADER_BYTES 0x800
#define EXE_IDENTIFIER "PS-X EXE"
#define EXE_TEXT_ADDRESS 0x18
#define EXE_TEXT_BYTES 0x1C
#define WORD_BYTES 4
#define HALF_BYTES 2
#define HALF_BITS 16
#define HALF_MASK 0xFFFFu

/* The hardware's registers the libraries reach */
#define SPU_START 0x1F801C00u
#define SPU_END 0x1F802000u
#define SPU_CONTROL 0x1F801DAAu
#define SPU_TRANSFER_ADDRESS 0x1F801DA6u
#define SPU_ADDRESS_SHIFT 3
#define SPU_TRANSFER_MODE_SHIFT 4
#define SPU_TRANSFER_MODE_MASK 3u
#define SPU_TRANSFER_DMA_WRITE 2u
/* The memory control's registers (the buses' delays and sizes, the RAM's size), which mean nothing on the host: kept as
 * written */
#define MEMORY_CONTROL_START 0x1F801000u
#define MEMORY_CONTROL_END 0x1F801064u
#define INTERRUPT_STATUS 0x1F801070u
#define INTERRUPT_MASK 0x1F801074u
#define DMA_START 0x1F801080u
#define DMA_END 0x1F801100u
#define DMA_CHANNEL_BYTES 0x10
#define DMA_SPU 4
#define DMA_ADDRESS 0x0
#define DMA_BLOCK 0x4
#define DMA_CONTROL 0x8
#define DMA_INTERRUPT 0x1F8010F4u
/* A DMA channel's control: from RAM to the device, and busy (started); its block's size and count */
#define DMA_FROM_RAM 1u
#define DMA_BUSY (1u << 24)
#define DMA_BLOCK_SIZE_MASK 0xFFFFu
#define DMA_BLOCK_COUNT_SHIFT 16
/* The DMA interrupt register: a channel's enable bit from 16, its flag from 24, the master flag at 31 */
#define DMA_INTERRUPT_ENABLE_SHIFT 16
#define DMA_INTERRUPT_FLAG_SHIFT 24
#define DMA_INTERRUPT_MASTER (1u << 31)
#define DMA_INTERRUPT_FLAGS 0x7F000000u
#define DMA_CHANNELS 8
#define IRQ_DMA (1u << 3)
#define VIDEO_MODE_NTSC 0
#define CRITICAL_ENTERED 1
/* libetc's interrupt callbacks: by interrupt, of which the host delivers the vertical blank's */
#define CALLBACK_CHANNELS 16
#define IRQ_VERTICAL_BLANK 0
#define PRINT_BYTES 256
/* TRADEOFF: the CPU's time as the libraries run, which the SPU's transfers and status take, counted as so many cycles
 * a read of the SPU's registers; their waits on the SPU's status read it in a loop */
#define SPU_READ_CYCLES 64

static uint32_t s_InterruptStatus;
static uint32_t s_InterruptMask;
static uint32_t s_MemoryControl[(MEMORY_CONTROL_END - MEMORY_CONTROL_START) / WORD_BYTES];
static uint32_t s_Dma[(DMA_END - DMA_START) / WORD_BYTES];
static uint32_t s_VSyncCallback;
static uint32_t s_DmaCallback[DMA_CHANNELS];
static uint32_t s_InterruptCallback[CALLBACK_CHANNELS];
static int s_Critical;

static uint32_t *dma_register(uint32_t address) {
    return &s_Dma[(address - DMA_START) / WORD_BYTES];
}

/* The sound's DMA, from RAM to the SPU's RAM at its transfer address, at once.
 * TRADEOFF: the SPU's 32-halfword FIFO and the transfer's time left out; the libraries wait for its end only */
static void spu_dma(void) {
    uint32_t base = DMA_START + DMA_SPU * DMA_CHANNEL_BYTES;
    uint32_t control = *dma_register(base + DMA_CONTROL);
    uint32_t block = *dma_register(base + DMA_BLOCK);
    uint32_t words = (block & DMA_BLOCK_SIZE_MASK) * (block >> DMA_BLOCK_COUNT_SHIFT);
    uint32_t source = *dma_register(base + DMA_ADDRESS);
    uint32_t mode = (Spu_ReadRegister(SPU_CONTROL) >> SPU_TRANSFER_MODE_SHIFT) & SPU_TRANSFER_MODE_MASK;
    uint32_t target = (uint32_t)Spu_ReadRegister(SPU_TRANSFER_ADDRESS) << SPU_ADDRESS_SHIFT;
    uint32_t index;

    if (!(control & DMA_FROM_RAM) || mode != SPU_TRANSFER_DMA_WRITE) {
        Host_Fatal("the SPU's DMA 0x%08X with the SPU in transfer mode %u: only writes to its RAM are known", control,
                   mode);
    } else {
        for (index = 0; index < words; index++) {
            uint32_t word = Mips_Read(source + index * WORD_BYTES, WORD_BYTES);

            Spu_WriteRam(target + index * WORD_BYTES, &word, WORD_BYTES);
        }
        *dma_register(base + DMA_CONTROL) = control & ~DMA_BUSY;
    }
}

/* The DMA's end as its interrupt: the flag set, and libetc's callback for the channel run once interrupts are on */
static void deliver_dma(void) {
    uint32_t *interrupt = dma_register(DMA_INTERRUPT);

    if (s_Critical) {
        Mips_SetPending(deliver_dma);
        return;
    } else {
        /* Interrupts on */
    }
    if (*interrupt & (1u << (DMA_INTERRUPT_ENABLE_SHIFT + DMA_SPU))) {
        *interrupt |= (1u << (DMA_INTERRUPT_FLAG_SHIFT + DMA_SPU)) | DMA_INTERRUPT_MASTER;
        s_InterruptStatus |= IRQ_DMA;
    } else {
        /* The channel's interrupt off */
    }
    if (s_DmaCallback[DMA_SPU] != 0) {
        Mips_Call(s_DmaCallback[DMA_SPU], NULL, 0);
    } else {
        /* No callback */
    }
}

static uint32_t io_read(uint32_t address, int bytes) {
    if (address >= SPU_START && address < SPU_END && bytes == HALF_BYTES) {
        Spu_Advance(SPU_READ_CYCLES);
        return Spu_ReadRegister(address);
    } else if (address >= SPU_START && address < SPU_END && bytes == WORD_BYTES) {
        Spu_Advance(SPU_READ_CYCLES);
        return Spu_ReadRegister(address) | ((uint32_t)Spu_ReadRegister(address + HALF_BYTES) << HALF_BITS);
    } else if (address == INTERRUPT_STATUS) {
        return s_InterruptStatus;
    } else if (address == INTERRUPT_MASK) {
        return s_InterruptMask;
    } else if (address >= DMA_START && address < DMA_END && bytes == WORD_BYTES) {
        return *dma_register(address);
    } else if (address >= MEMORY_CONTROL_START && address < MEMORY_CONTROL_END && bytes == WORD_BYTES) {
        return s_MemoryControl[(address - MEMORY_CONTROL_START) / WORD_BYTES];
    } else {
        Host_Fatal("the sound libraries read the hardware at 0x%08X (%d bytes), which the host doesn't have", address,
                   bytes);
        return 0;
    }
}

static void io_write(uint32_t address, uint32_t value, int bytes) {
    if (address >= SPU_START && address < SPU_END && bytes == HALF_BYTES) {
        Spu_WriteRegister(address, (uint16_t)value);
    } else if (address >= SPU_START && address < SPU_END && bytes == WORD_BYTES) {
        Spu_WriteRegister(address, (uint16_t)(value & HALF_MASK));
        Spu_WriteRegister(address + HALF_BYTES, (uint16_t)(value >> HALF_BITS));
    } else if (address == INTERRUPT_STATUS) {
        /* Writing 0s acknowledges */
        s_InterruptStatus &= value;
    } else if (address == INTERRUPT_MASK) {
        s_InterruptMask = value;
    } else if (address == DMA_INTERRUPT && bytes == WORD_BYTES) {
        /* Writing 1s to the flags acknowledges them */
        *dma_register(address) = (value & ~DMA_INTERRUPT_FLAGS & ~DMA_INTERRUPT_MASTER) |
                                 (*dma_register(address) & DMA_INTERRUPT_FLAGS & ~value);
    } else if (address == DMA_START + DMA_SPU * DMA_CHANNEL_BYTES + DMA_CONTROL && bytes == WORD_BYTES) {
        *dma_register(address) = value;
        if (value & DMA_BUSY) {
            spu_dma();
            Mips_SetPending(deliver_dma);
        } else {
            /* Stopped */
        }
    } else if (address >= DMA_START && address < DMA_END && bytes == WORD_BYTES) {
        *dma_register(address) = value;
    } else if (address >= MEMORY_CONTROL_START && address < MEMORY_CONTROL_END && bytes == WORD_BYTES) {
        s_MemoryControl[(address - MEMORY_CONTROL_START) / WORD_BYTES] = value;
    } else {
        Host_Fatal("the sound libraries write 0x%X to the hardware at 0x%08X (%d bytes), which the host doesn't have",
                   value, address, bytes);
    }
}

static const MipsIo s_Io = {io_read, io_write};

/* The system's functions the libraries call, by libapi's and libetc's documented behaviour */
static void enter_critical_section(MipsCpu *cpu) {
    cpu->regs[MIPS_REG_V0] = s_Critical ? 0 : CRITICAL_ENTERED;
    s_Critical = 1;
}

static void exit_critical_section(MipsCpu *cpu) {
    (void)cpu;
    s_Critical = 0;
}

static void vsync_callback(MipsCpu *cpu) {
    s_VSyncCallback = Mips_Argument(cpu, 0);
}

static void dma_callback(MipsCpu *cpu) {
    uint32_t channel = Mips_Argument(cpu, 0);

    if (channel >= DMA_CHANNELS) {
        Host_Fatal("DMACallback for channel %u", channel);
    } else {
        cpu->regs[MIPS_REG_V0] = s_DmaCallback[channel];
        s_DmaCallback[channel] = Mips_Argument(cpu, 1);
    }
}

static void interrupt_callback(MipsCpu *cpu) {
    uint32_t irq = Mips_Argument(cpu, 0);

    if (irq >= CALLBACK_CHANNELS || (irq != IRQ_VERTICAL_BLANK && Mips_Argument(cpu, 1) != 0)) {
        Host_Fatal("InterruptCallback for interrupt %u, which the host doesn't deliver", irq);
    } else {
        cpu->regs[MIPS_REG_V0] = s_InterruptCallback[irq];
        s_InterruptCallback[irq] = Mips_Argument(cpu, 1);
    }
}

static void reset_callback(MipsCpu *cpu) {
    cpu->regs[MIPS_REG_V0] = 0;
}

static void get_video_mode(MipsCpu *cpu) {
    cpu->regs[MIPS_REG_V0] = VIDEO_MODE_NTSC;
}

/* TRADEOFF: the format logged, its arguments left out; the libraries print only on errors */
static void print(MipsCpu *cpu) {
    char text[PRINT_BYTES];
    uint32_t address = Mips_Argument(cpu, 0);
    size_t index;

    for (index = 0; index + 1 < sizeof(text); index++) {
        text[index] = (char)Mips_Read(address + (uint32_t)index, 1);
        if (text[index] == '\0') {
            break;
        } else {
            continue;
        }
    }
    text[index] = '\0';
    Host_Log("sound libraries: %s", text);
    cpu->regs[MIPS_REG_V0] = 0;
}

/* libapi's events and root counters: events.c's, which the game's C shares */
static void open_event(MipsCpu *cpu) {
    cpu->regs[MIPS_REG_V0] = (uint32_t)Host_OpenRetailEvent(Mips_Argument(cpu, 0), (int)Mips_Argument(cpu, 1),
                                                            (int)Mips_Argument(cpu, 2), Mips_Argument(cpu, 3));
}

static void enable_event(MipsCpu *cpu) {
    cpu->regs[MIPS_REG_V0] = (uint32_t)EnableEvent((int)Mips_Argument(cpu, 0));
}

static void deliver_event(MipsCpu *cpu) {
    DeliverEvent(Mips_Argument(cpu, 0), (int)Mips_Argument(cpu, 1));
}

static void test_event(MipsCpu *cpu) {
    cpu->regs[MIPS_REG_V0] = (uint32_t)TestEvent((int)Mips_Argument(cpu, 0));
}

static void wait_event(MipsCpu *cpu) {
    cpu->regs[MIPS_REG_V0] = (uint32_t)WaitEvent((int)Mips_Argument(cpu, 0));
}

static void set_root_counter(MipsCpu *cpu) {
    cpu->regs[MIPS_REG_V0] = (uint32_t)SetRCnt(Mips_Argument(cpu, 0), (unsigned short)Mips_Argument(cpu, 1),
                                               (int)Mips_Argument(cpu, 2));
}

static void reset_root_counter(MipsCpu *cpu) {
    cpu->regs[MIPS_REG_V0] = (uint32_t)ResetRCnt(Mips_Argument(cpu, 0));
}

void Host_SoundCall(unsigned int address) {
    Mips_Call(address, NULL, 0);
}

void Host_SoundStart(void) {
    unsigned int bytes;
    uint8_t *file = Host_ReadDiscFile(EXECUTABLE, &bytes);
    uint32_t text_address, text_bytes;

    memcpy(&text_address, file + EXE_TEXT_ADDRESS, WORD_BYTES);
    memcpy(&text_bytes, file + EXE_TEXT_BYTES, WORD_BYTES);
    if (bytes < EXE_HEADER_BYTES || memcmp(file, EXE_IDENTIFIER, strlen(EXE_IDENTIFIER)) != 0 ||
        text_address != RETAIL_CODE_START || text_bytes < RETAIL_DATA_START - RETAIL_CODE_START ||
        text_bytes > bytes - EXE_HEADER_BYTES) {
        Host_Fatal("the disc's %s isn't the retail executable", EXECUTABLE);
    } else {
        Mips_SetImage(file + EXE_HEADER_BYTES, text_bytes);
        Mips_SetIo(&s_Io);
    }
    Mips_AddHook(SDK_EnterCriticalSection, enter_critical_section);
    Mips_AddHook(SDK_ExitCriticalSection, exit_critical_section);
    Mips_AddHook(SDK_VSyncCallback, vsync_callback);
    Mips_AddHook(SDK_DMACallback, dma_callback);
    Mips_AddHook(SDK_InterruptCallback, interrupt_callback);
    Mips_AddHook(SDK_ResetCallback, reset_callback);
    Mips_AddHook(SDK_GetVideoMode, get_video_mode);
    Mips_AddHook(SDK_printf, print);
    Mips_AddHook(SDK_SetRCnt, set_root_counter);
    Mips_AddHook(SDK_ResetRCnt, reset_root_counter);
    Mips_AddHook(SDK_OpenEvent, open_event);
    Mips_AddHook(SDK_EnableEvent, enable_event);
    Mips_AddHook(SDK_DeliverEvent, deliver_event);
    Mips_AddHook(SDK_TestEvent, test_event);
    Mips_AddHook(SDK_WaitEvent, wait_event);
    Spu_Init();
}

/* A vertical blank's interrupt: its interrupt callback (libsnd's tick, in the game's tick mode) and libetc's VSync
 * callback */
void Host_SoundVerticalBlank(void) {
    if (s_InterruptCallback[IRQ_VERTICAL_BLANK] != 0) {
        Mips_Call(s_InterruptCallback[IRQ_VERTICAL_BLANK], NULL, 0);
    } else {
        /* None set */
    }
    if (s_VSyncCallback != 0) {
        Mips_Call(s_VSyncCallback, NULL, 0);
    } else {
        /* None set */
    }
}

/* The game's calls into the libraries, their arguments as the PlayStation's caller passes them (a short sign-extended,
 * an unsigned char zero-extended) */
static uint32_t call0(uint32_t address) {
    return Mips_Call(address, NULL, 0);
}

static uint32_t call1(uint32_t address, uint32_t a0) {
    uint32_t arguments[] = {a0};

    return Mips_Call(address, arguments, 1);
}

static uint32_t call2(uint32_t address, uint32_t a0, uint32_t a1) {
    uint32_t arguments[] = {a0, a1};

    return Mips_Call(address, arguments, 2);
}

static uint32_t call3(uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t arguments[] = {a0, a1, a2};

    return Mips_Call(address, arguments, 3);
}

static uint32_t call4(uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t arguments[] = {a0, a1, a2, a3};

    return Mips_Call(address, arguments, 4);
}

#define POINTER(pointer) ((uint32_t)(uintptr_t)(pointer))
#define SHORT(value) ((uint32_t)(int32_t)(int16_t)(value))

/* libsnd */
void SsInit(void) {
    call0(SDK_SsInit);
}

void SsSetTableSize(char *table, int16_t seqs, int16_t seps) {
    call3(SDK_SsSetTableSize, POINTER(table), SHORT(seqs), SHORT(seps));
}

void SsSetTickMode(int32_t mode) {
    call1(SDK_SsSetTickMode, (uint32_t)mode);
}

void SsStart(void) {
    call0(SDK_SsStart);
}

void SsEnd(void) {
    call0(SDK_SsEnd);
}

void SsSetReservedVoice(uint8_t voices) {
    call1(SDK_SsSetReservedVoice, voices);
}

void SsSetMVol(int16_t left, int16_t right) {
    call2(SDK_SsSetMVol, SHORT(left), SHORT(right));
}

int16_t SsVabFakeHead(const void *header, int16_t vab, uint32_t sample_base) {
    return (int16_t)call3(SDK_SsVabFakeHead, POINTER(header), SHORT(vab), sample_base);
}

int16_t SsVabFakeBody(int16_t vab) {
    return (int16_t)call1(SDK_SsVabFakeBody, SHORT(vab));
}

void SsVabClose(int16_t vab) {
    call1(SDK_SsVabClose, SHORT(vab));
}

int16_t SsSepOpen(const void *sep, int16_t vab, int16_t count) {
    return (int16_t)call3(SDK_SsSepOpen, POINTER(sep), SHORT(vab), SHORT(count));
}

void SsSepClose(int16_t access) {
    call1(SDK_SsSepClose, SHORT(access));
}

void SsSepPlay(int16_t access, int16_t seq, char mode, int16_t plays) {
    call4(SDK_SsSepPlay, SHORT(access), SHORT(seq), (uint32_t)(int32_t)mode, SHORT(plays));
}

void SsSepPause(int16_t access, int16_t seq) {
    call2(SDK_SsSepPause, SHORT(access), SHORT(seq));
}

void SsSepReplay(int16_t access, int16_t seq) {
    call2(SDK_SsSepReplay, SHORT(access), SHORT(seq));
}

/* The tick libsnd's data names, D_8005EFE0 (the interpreter reaches the retail one through it) */
void SsSeqCalledTbyT(void) {
    call0(SDK_SsSeqCalledTbyT);
}

/* SsSepStop */
void func_80056304(int16_t access, int16_t seq) {
    call2(SDK_func_80056304, SHORT(access), SHORT(seq));
}

/* SsSepSetVol */
void func_80056760(int16_t access, int16_t seq, int16_t left, int16_t right) {
    call4(SDK_func_80056760, SHORT(access), SHORT(seq), SHORT(left), SHORT(right));
}

/* SsSepSetCrescendo */
void func_800528C4(int16_t access, int16_t seq, int32_t volume, int32_t ticks) {
    call4(SDK_func_800528C4, SHORT(access), SHORT(seq), (uint32_t)volume, (uint32_t)ticks);
}

/* SsSepSetDecrescendo */
void func_80052B4C(int16_t access, int16_t seq, int32_t volume, int32_t ticks) {
    call4(SDK_func_80052B4C, SHORT(access), SHORT(seq), (uint32_t)volume, (uint32_t)ticks);
}

/* libspu: the 10 functions the game calls by addresses the split leaves unnamed are named by their arguments and uses
 * in decomp's src/game/800324A8.c and 800123F0.c */

/* SpuSetKey */
void func_80051078(int32_t on, uint32_t voices) {
    call2(SDK_func_80051078, (uint32_t)on, voices);
}

/* SpuGetAllKeysStatus */
void func_8005182C(char *status) {
    call1(SDK_func_8005182C, POINTER(status));
}

/* SpuSetVoiceAttr */
void func_800518B8(const void *attributes) {
    call1(SDK_func_800518B8, POINTER(attributes));
}

/* SpuGetVoiceAttr */
void func_800521A8(void *attributes) {
    call1(SDK_func_800521A8, POINTER(attributes));
}

/* SpuSetCommonAttr */
void func_800513D8(const void *attributes) {
    call1(SDK_func_800513D8, POINTER(attributes));
}

/* SpuSetTransferMode */
int32_t func_800512F8(int32_t mode) {
    return (int32_t)call1(SDK_func_800512F8, (uint32_t)mode);
}

/* SpuSetTransferStartAddr */
uint32_t func_80051298(uint32_t address) {
    return call1(SDK_func_80051298, address);
}

/* SpuWrite */
uint32_t func_80051238(const void *data, uint32_t bytes) {
    return call2(SDK_func_80051238, POINTER(data), bytes);
}

/* SpuIsTransferCompleted */
int32_t func_80051328(int32_t mode) {
    return (int32_t)call1(SDK_func_80051328, (uint32_t)mode);
}

int32_t SpuInitMalloc(int32_t blocks, char *table) {
    return (int32_t)call2(SDK_SpuInitMalloc, (uint32_t)blocks, POINTER(table));
}

/* SpuSetReverbModeParam */
void func_800501A8(const void *attributes) {
    call1(SDK_func_800501A8, POINTER(attributes));
}

void SpuSetReverbDepth(const void *attributes) {
    call1(SDK_SpuSetReverbDepth, POINTER(attributes));
}

/* SpuSetReverb */
int32_t func_8004FE38(int32_t on) {
    return (int32_t)call1(SDK_func_8004FE38, (uint32_t)on);
}

uint32_t SpuSetReverbVoice(int32_t on, uint32_t voices) {
    return call2(SDK_SpuSetReverbVoice, (uint32_t)on, voices);
}
