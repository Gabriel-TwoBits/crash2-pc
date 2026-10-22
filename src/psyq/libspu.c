#include <stdint.h>
#include <string.h>

#include "psyq/host_psyq.h"
#include "spu_host.h"

/* libspu on the SPU (spu_host.h): the functions the game and libsnd call, from libspu's documented behaviour, as
 * writes to the SPU's registers (psx-spx, "Sound Processing Unit"). The game calls 10 of them by addresses the split
 * leaves unnamed; their names here come from their arguments and uses in decomp's src/game/800324A8.c and
 * 800123F0.c, the structures' layouts from how the game fills them */

/* The SPU's registers */
#define SPU_VOICE_BASE 0x1F801C00u
#define SPU_VOICE_STRIDE 0x10
#define VOICE_VOLUME_LEFT 0x0
#define VOICE_VOLUME_RIGHT 0x2
#define VOICE_PITCH 0x4
#define VOICE_START 0x6
#define VOICE_ADSR_LOW 0x8
#define VOICE_ADSR_HIGH 0xA
#define VOICE_ENVELOPE 0xC
#define VOICE_LOOP 0xE
#define SPU_MAIN_VOLUME_LEFT 0x1F801D80u
#define SPU_MAIN_VOLUME_RIGHT 0x1F801D82u
#define SPU_REVERB_VOLUME_LEFT 0x1F801D84u
#define SPU_REVERB_VOLUME_RIGHT 0x1F801D86u
#define SPU_KEY_ON_LOW 0x1F801D88u
#define SPU_KEY_ON_HIGH 0x1F801D8Au
#define SPU_KEY_OFF_LOW 0x1F801D8Cu
#define SPU_KEY_OFF_HIGH 0x1F801D8Eu
#define SPU_REVERB_ON_LOW 0x1F801D98u
#define SPU_REVERB_ON_HIGH 0x1F801D9Au
#define SPU_REVERB_BASE 0x1F801DA2u
#define SPU_CONTROL 0x1F801DAAu
#define SPU_CD_VOLUME_LEFT 0x1F801DB0u
#define SPU_CD_VOLUME_RIGHT 0x1F801DB2u
#define SPU_REVERB_REGISTERS 0x1F801DC0u
#define REVERB_REGISTERS 32
#define REGISTER_BYTES 2
/* The control register: enabled, unmuted; the reverb's master enable */
#define CONTROL_ENABLE_UNMUTE 0xC000
#define CONTROL_REVERB 0x0080
#define VOICES 24
#define HALF_MASK 0xFFFF
#define HALF_SHIFT 16
/* A volume in fixed mode: 15 bits, its top one clear */
#define VOLUME_MASK 0x7FFF
#define ADDRESS_SHIFT 3
#define SPU_RAM_BYTES 0x80000u
#define CLEAR_CHUNK_BYTES 0x1000u

/* SpuVoiceAttr's mask bits (the game sets 0xFF93: both volumes, the pitch, the start and the eight ADSR fields) */
#define VOICE_ATTR_VOLUME_LEFT (1u << 0)
#define VOICE_ATTR_VOLUME_RIGHT (1u << 1)
#define VOICE_ATTR_PITCH (1u << 4)
#define VOICE_ATTR_START (1u << 7)
#define VOICE_ATTR_ATTACK_MODE (1u << 8)
#define VOICE_ATTR_SUSTAIN_MODE (1u << 9)
#define VOICE_ATTR_RELEASE_MODE (1u << 10)
#define VOICE_ATTR_ATTACK_RATE (1u << 11)
#define VOICE_ATTR_DECAY_RATE (1u << 12)
#define VOICE_ATTR_SUSTAIN_RATE (1u << 13)
#define VOICE_ATTR_RELEASE_RATE (1u << 14)
#define VOICE_ATTR_SUSTAIN_LEVEL (1u << 15)
#define VOICE_ATTR_LOOP (1u << 16)
#define VOICE_ATTR_ADSR1 (1u << 17)
#define VOICE_ATTR_ADSR2 (1u << 18)
#define VOICE_ATTR_ADSR_FIELDS 0xFF00u

/* libspu's envelope modes, and their bits in the ADSR registers (psx-spx, "ADSR1", "ADSR2") */
enum {
    MODE_LINEAR_INCREASE = 1,
    MODE_LINEAR_INCREASE_REVERSE = 2,
    MODE_LINEAR_DECREASE = 3,
    MODE_LINEAR_DECREASE_REVERSE = 4,
    MODE_EXPONENTIAL_INCREASE = 5,
    MODE_EXPONENTIAL_INCREASE_REVERSE = 6,
    MODE_EXPONENTIAL_DECREASE = 7
};
#define ADSR_ATTACK_EXPONENTIAL 0x8000
#define ADSR_ATTACK_SHIFT 8
#define ADSR_ATTACK_MASK 0x7F
#define ADSR_DECAY_SHIFT 4
#define ADSR_DECAY_MASK 0xF
#define ADSR_SUSTAIN_LEVEL_MASK 0xF
#define ADSR_SUSTAIN_EXPONENTIAL 0x8000
#define ADSR_SUSTAIN_DECREASE 0x4000
#define ADSR_SUSTAIN_SHIFT 6
#define ADSR_SUSTAIN_MASK 0x7F
#define ADSR_RELEASE_EXPONENTIAL 0x0020
#define ADSR_RELEASE_MASK 0x1F

/* SpuCommonAttr's mask bits: the main volume (the game sets 3), the CD's */
#define COMMON_MAIN_LEFT (1u << 0)
#define COMMON_MAIN_RIGHT (1u << 1)
#define COMMON_CD_LEFT (1u << 6)
#define COMMON_CD_RIGHT (1u << 7)

/* SpuReverbAttr's mask bits (the game sets 7), and the mode's flag asking for the work area cleared */
#define REVERB_ATTR_MODE (1u << 0)
#define REVERB_ATTR_DEPTH_LEFT (1u << 1)
#define REVERB_ATTR_DEPTH_RIGHT (1u << 2)
#define REVERB_MODE_CLEAR 0x100
#define REVERB_MODE_MASK 0xFF

/* SpuGetAllKeysStatus's states */
enum {
    KEY_OFF = 0,
    KEY_ON = 1,
    KEY_OFF_ENVELOPE_ON = 2,
    KEY_ON_ENVELOPE_OFF = 3
};

#define TRANSFER_DONE 1

typedef struct {
    int16_t left, right;
} SpuVolume;

typedef struct {
    uint32_t voice;
    uint32_t mask;
    SpuVolume volume;
    SpuVolume volmode;
    SpuVolume volumex;
    uint16_t pitch;
    uint16_t note;
    uint16_t sample_note;
    int16_t envx;
    uint32_t addr;
    uint32_t loop_addr;
    int32_t a_mode, s_mode, r_mode;
    uint16_t ar, dr, sr, rr, sl;
    uint16_t adsr1, adsr2;
} SpuVoiceAttr;

typedef struct {
    uint32_t mask;
    SpuVolume mvol;
    SpuVolume mvolmode;
    SpuVolume mvolx;
    SpuVolume cd_volume;
    int32_t cd_reverb;
    int32_t cd_mix;
} SpuCommonAttr;

typedef struct {
    uint32_t mask;
    int32_t mode;
    SpuVolume depth;
    int32_t delay;
    int32_t feedback;
} SpuReverbAttr;

/* libspu's reverb modes as psx-spx's examples give their registers and work area (its "SPU Reverb Examples"): off,
 * room, studio small, medium and large, hall, space echo, echo, delay, pipe.
 * TRADEOFF: the echo's and delay's own delay and feedback parameters aren't applied (undocumented); pipe takes the
 * "half echo" example */
#define REVERB_MODES 10
static const uint32_t s_ReverbSizes[REVERB_MODES] = {0x10,   0x26C0, 0x1F40,  0x4840,  0x6FE0,
                                                     0xADE0, 0xF6C0, 0x18040, 0x18040, 0x3C00};
static const uint16_t s_ReverbPresets[REVERB_MODES][REVERB_REGISTERS] = {
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
     0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001,
     0x0001, 0x0001, 0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0000, 0x0000},
    {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6,
     0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000},
    {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4,
     0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135,
     0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000},
    {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904,
     0x076B, 0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305,
     0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000},
    {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB,
     0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0,
     0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000},
    {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA,
     0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1,
     0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
    {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6,
     0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D,
     0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000},
    {0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFF,
     0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000},
    {0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFF,
     0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000},
    {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371,
     0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7, 0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1,
     0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
};

static int s_Started;
static uint32_t s_TransferAddress;
static uint32_t s_KeyedOn;
static uint16_t s_Control;
static uint32_t s_ReverbVoices;

/* The SPU set up as libspu's SpuInit leaves it: enabled, unmuted, no voice keyed, no reverb */
void Host_SpuStart(void) {
    if (s_Started) {
        return;
    } else {
        s_Started = 1;
        Spu_Init();
        s_Control = CONTROL_ENABLE_UNMUTE;
        Spu_WriteRegister(SPU_CONTROL, s_Control);
        Spu_WriteRegister(SPU_KEY_OFF_LOW, HALF_MASK);
        Spu_WriteRegister(SPU_KEY_OFF_HIGH, (uint16_t)((1u << (VOICES - HALF_SHIFT)) - 1));
    }
}

static void write_pair(uint32_t low_register, uint32_t high_register, uint32_t bits) {
    Spu_WriteRegister(low_register, (uint16_t)(bits & HALF_MASK));
    Spu_WriteRegister(high_register, (uint16_t)(bits >> HALF_SHIFT));
}

static uint32_t voice_register(int voice, uint32_t offset) {
    return SPU_VOICE_BASE + (uint32_t)voice * SPU_VOICE_STRIDE + offset;
}

/* SpuSetKey: the voices' key on or off */
void func_80051078(int on, uint32_t voices) {
    Host_SpuStart();
    if (on) {
        s_KeyedOn |= voices;
        write_pair(SPU_KEY_ON_LOW, SPU_KEY_ON_HIGH, voices);
    } else {
        s_KeyedOn &= ~voices;
        write_pair(SPU_KEY_OFF_LOW, SPU_KEY_OFF_HIGH, voices);
    }
}

/* SpuGetAllKeysStatus: each voice's key and envelope */
void func_8005182C(uint8_t *status) {
    int voice;

    Host_SpuStart();
    for (voice = 0; voice < VOICES; voice++) {
        int keyed = (s_KeyedOn >> voice) & 1;
        int sounding = Spu_ReadRegister(voice_register(voice, VOICE_ENVELOPE)) != 0;

        if (keyed && sounding) {
            status[voice] = KEY_ON;
        } else if (keyed) {
            status[voice] = KEY_ON_ENVELOPE_OFF;
        } else if (sounding) {
            status[voice] = KEY_OFF_ENVELOPE_ON;
        } else {
            status[voice] = KEY_OFF;
        }
    }
}

static int exponential(int32_t mode) {
    return mode == MODE_EXPONENTIAL_INCREASE || mode == MODE_EXPONENTIAL_INCREASE_REVERSE ||
           mode == MODE_EXPONENTIAL_DECREASE;
}

static int decreasing(int32_t mode) {
    return mode == MODE_LINEAR_DECREASE || mode == MODE_LINEAR_DECREASE_REVERSE || mode == MODE_EXPONENTIAL_DECREASE;
}

static uint16_t with_field(uint16_t value, uint16_t field, uint16_t bits) {
    return (uint16_t)((value & ~field) | (bits & field));
}

/* The ADSR registers' fields the attributes set, over their current values */
static void set_envelope(int voice, const SpuVoiceAttr *attr) {
    uint16_t low = Spu_ReadRegister(voice_register(voice, VOICE_ADSR_LOW));
    uint16_t high = Spu_ReadRegister(voice_register(voice, VOICE_ADSR_HIGH));

    if (attr->mask & VOICE_ATTR_ATTACK_MODE) {
        low = with_field(low, ADSR_ATTACK_EXPONENTIAL, exponential(attr->a_mode) ? ADSR_ATTACK_EXPONENTIAL : 0);
    } else {
        /* The attack's mode kept */
    }
    if (attr->mask & VOICE_ATTR_ATTACK_RATE) {
        low = with_field(low, ADSR_ATTACK_MASK << ADSR_ATTACK_SHIFT, (uint16_t)(attr->ar << ADSR_ATTACK_SHIFT));
    } else {
        /* The attack's rate kept */
    }
    if (attr->mask & VOICE_ATTR_DECAY_RATE) {
        low = with_field(low, ADSR_DECAY_MASK << ADSR_DECAY_SHIFT, (uint16_t)(attr->dr << ADSR_DECAY_SHIFT));
    } else {
        /* The decay's rate kept */
    }
    if (attr->mask & VOICE_ATTR_SUSTAIN_LEVEL) {
        low = with_field(low, ADSR_SUSTAIN_LEVEL_MASK, attr->sl);
    } else {
        /* The sustain's level kept */
    }
    if (attr->mask & VOICE_ATTR_SUSTAIN_MODE) {
        high = with_field(high, ADSR_SUSTAIN_EXPONENTIAL | ADSR_SUSTAIN_DECREASE,
                          (uint16_t)((exponential(attr->s_mode) ? ADSR_SUSTAIN_EXPONENTIAL : 0) |
                                     (decreasing(attr->s_mode) ? ADSR_SUSTAIN_DECREASE : 0)));
    } else {
        /* The sustain's mode kept */
    }
    if (attr->mask & VOICE_ATTR_SUSTAIN_RATE) {
        high = with_field(high, ADSR_SUSTAIN_MASK << ADSR_SUSTAIN_SHIFT, (uint16_t)(attr->sr << ADSR_SUSTAIN_SHIFT));
    } else {
        /* The sustain's rate kept */
    }
    if (attr->mask & VOICE_ATTR_RELEASE_MODE) {
        high = with_field(high, ADSR_RELEASE_EXPONENTIAL, exponential(attr->r_mode) ? ADSR_RELEASE_EXPONENTIAL : 0);
    } else {
        /* The release's mode kept */
    }
    if (attr->mask & VOICE_ATTR_RELEASE_RATE) {
        high = with_field(high, ADSR_RELEASE_MASK, attr->rr);
    } else {
        /* The release's rate kept */
    }
    Spu_WriteRegister(voice_register(voice, VOICE_ADSR_LOW), low);
    Spu_WriteRegister(voice_register(voice, VOICE_ADSR_HIGH), high);
}

/* One voice's register from the attributes, when the mask names it */
static void set_if(const SpuVoiceAttr *attr, uint32_t bit, int voice, uint32_t offset, uint16_t value) {
    if (attr->mask & bit) {
        Spu_WriteRegister(voice_register(voice, offset), value);
    } else {
        /* Kept */
    }
}

/* SpuSetVoiceAttr: the attributes the mask names, on each voice named */
void func_800518B8(const SpuVoiceAttr *attr) {
    int voice;

    Host_SpuStart();
    for (voice = 0; voice < VOICES; voice++) {
        if (!((attr->voice >> voice) & 1)) {
            continue;
        } else {
            set_if(attr, VOICE_ATTR_VOLUME_LEFT, voice, VOICE_VOLUME_LEFT, (uint16_t)(attr->volume.left & VOLUME_MASK));
            set_if(attr, VOICE_ATTR_VOLUME_RIGHT, voice, VOICE_VOLUME_RIGHT,
                   (uint16_t)(attr->volume.right & VOLUME_MASK));
            set_if(attr, VOICE_ATTR_PITCH, voice, VOICE_PITCH, attr->pitch);
            set_if(attr, VOICE_ATTR_START, voice, VOICE_START, (uint16_t)(attr->addr >> ADDRESS_SHIFT));
            set_if(attr, VOICE_ATTR_LOOP, voice, VOICE_LOOP, (uint16_t)(attr->loop_addr >> ADDRESS_SHIFT));
        }
        if (attr->mask & VOICE_ATTR_ADSR_FIELDS) {
            set_envelope(voice, attr);
        } else {
            /* The envelope kept */
        }
        set_if(attr, VOICE_ATTR_ADSR1, voice, VOICE_ADSR_LOW, attr->adsr1);
        set_if(attr, VOICE_ATTR_ADSR2, voice, VOICE_ADSR_HIGH, attr->adsr2);
    }
}

/* SpuGetVoiceAttr, of the one voice named: its volumes, the only attributes the game reads (Snd_AllocVoice) */
void func_800521A8(SpuVoiceAttr *attr) {
    int voice;

    Host_SpuStart();
    for (voice = 0; voice < VOICES && !((attr->voice >> voice) & 1); voice++) {
        /* The voice named */
    }
    if (voice < VOICES) {
        attr->volume.left = (int16_t)Spu_ReadRegister(voice_register(voice, VOICE_VOLUME_LEFT));
        attr->volume.right = (int16_t)Spu_ReadRegister(voice_register(voice, VOICE_VOLUME_RIGHT));
    } else {
        /* No voice named */
    }
}

/* SpuSetCommonAttr: the main and CD volumes the mask names */
void func_800513D8(const SpuCommonAttr *attr) {
    Host_SpuStart();
    if (attr->mask & COMMON_MAIN_LEFT) {
        Spu_WriteRegister(SPU_MAIN_VOLUME_LEFT, (uint16_t)(attr->mvol.left & VOLUME_MASK));
    } else {
        /* The main left volume kept */
    }
    if (attr->mask & COMMON_MAIN_RIGHT) {
        Spu_WriteRegister(SPU_MAIN_VOLUME_RIGHT, (uint16_t)(attr->mvol.right & VOLUME_MASK));
    } else {
        /* The main right volume kept */
    }
    if (attr->mask & COMMON_CD_LEFT) {
        Spu_WriteRegister(SPU_CD_VOLUME_LEFT, (uint16_t)attr->cd_volume.left);
    } else {
        /* The CD's left volume kept */
    }
    if (attr->mask & COMMON_CD_RIGHT) {
        Spu_WriteRegister(SPU_CD_VOLUME_RIGHT, (uint16_t)attr->cd_volume.right);
    } else {
        /* The CD's right volume kept */
    }
}

/* SpuSetTransferMode: every transfer completes at once here */
void func_800512F8(int mode) {
    (void)mode;
}

/* SpuSetTransferStartAddr */
uint32_t func_80051298(uint32_t address) {
    s_TransferAddress = address;
    return address;
}

/* SpuWrite: the bytes into the SPU's RAM from the transfer's address */
uint32_t func_80051238(const void *data, uint32_t bytes) {
    Host_SpuStart();
    Spu_WriteRam(s_TransferAddress, data, bytes);
    return bytes;
}

/* SpuIsTransferCompleted: the paging waits on it before and after a sound page's upload (decomp's
 * src/game/800123F0.c, Nsf_UpdatePageSlot) */
int func_80051328(int mode) {
    (void)mode;
    return TRANSFER_DONE;
}

int SpuInitMalloc(int blocks, void *table) {
    (void)table;
    return blocks;
}

/* SpuSetReverbModeParam: the mode's registers and work area at the top of the SPU's RAM, cleared when the mode asks */
void func_800501A8(const SpuReverbAttr *attr) {
    static const uint8_t zeros[CLEAR_CHUNK_BYTES];
    int mode = attr->mode & REVERB_MODE_MASK;
    uint32_t base;
    uint32_t address;
    int index;

    Host_SpuStart();
    if (!(attr->mask & REVERB_ATTR_MODE) || mode >= REVERB_MODES) {
        return;
    } else {
        base = SPU_RAM_BYTES - s_ReverbSizes[mode];
        Spu_WriteRegister(SPU_REVERB_BASE, (uint16_t)(base >> ADDRESS_SHIFT));
    }
    for (index = 0; index < REVERB_REGISTERS; index++) {
        Spu_WriteRegister(SPU_REVERB_REGISTERS + (uint32_t)index * REGISTER_BYTES, s_ReverbPresets[mode][index]);
    }
    for (address = base; (attr->mode & REVERB_MODE_CLEAR) && address < SPU_RAM_BYTES; address += CLEAR_CHUNK_BYTES) {
        uint32_t left = SPU_RAM_BYTES - address;

        Spu_WriteRam(address, zeros, left < CLEAR_CHUNK_BYTES ? left : CLEAR_CHUNK_BYTES);
    }
}

void SpuSetReverbDepth(const SpuReverbAttr *attr) {
    Host_SpuStart();
    if (attr->mask & REVERB_ATTR_DEPTH_LEFT) {
        Spu_WriteRegister(SPU_REVERB_VOLUME_LEFT, (uint16_t)attr->depth.left);
    } else {
        /* The left depth kept */
    }
    if (attr->mask & REVERB_ATTR_DEPTH_RIGHT) {
        Spu_WriteRegister(SPU_REVERB_VOLUME_RIGHT, (uint16_t)attr->depth.right);
    } else {
        /* The right depth kept */
    }
}

/* SpuSetReverb: the reverb's master enable */
void func_8004FE38(int on) {
    Host_SpuStart();
    if (on) {
        s_Control |= CONTROL_REVERB;
    } else {
        s_Control &= (uint16_t)~CONTROL_REVERB;
    }
    Spu_WriteRegister(SPU_CONTROL, s_Control);
}

/* SpuSetReverbVoice: the voices that feed the reverb, set or cleared */
uint32_t SpuSetReverbVoice(int on, uint32_t voices) {
    Host_SpuStart();
    if (on) {
        s_ReverbVoices |= voices;
    } else {
        s_ReverbVoices &= ~voices;
    }
    write_pair(SPU_REVERB_ON_LOW, SPU_REVERB_ON_HIGH, s_ReverbVoices);
    return s_ReverbVoices;
}
