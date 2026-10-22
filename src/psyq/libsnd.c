#include <math.h>
#include <stdint.h>
#include <string.h>

#include "host_log.h"
#include "psyq/host_psyq.h"
#include "spu_host.h"

/* libsnd's sequencer, written for the host from the formats psx-spx documents ("CDROM File Audio Sample Sets VAB" and
 * "Sequences SEQ/SEP"): the game's VABs (their headers in its memory, their samples already in the SPU's RAM, which
 * the paging uploads) and its SEPs' sequences, played as MIDI on the voices libsnd reserves (SsSetReservedVoice; the
 * game's sound effects take the others). The sequencer ticks at each vertical blank (SsSetTickMode 1, 60 a second).
 * The game calls 4 of its functions by addresses the split leaves unnamed: SsSepStop (0x80056304), SsSepSetVol
 * (0x80056760), SsSepSetCrescendo (0x800528C4) and SsSepSetDecrescendo (0x80052B4C), by their arguments and uses in
 * decomp's src/game/800324A8.c.
 * TRADEOFF: libsnd's own formulas (a note's volume, pan and pitch, voice allocation, vibrato and portamento) aren't
 * documented: these are MIDI's usual ones, and the tones' vibrato and portamento are left out */

#define VAB_MAX 16
#define SEP_MAX 4
#define SEQ_MAX 16
#define CHANNELS 16
#define VOICES 24
#define NOTE_NONE -1

/* VAB's layout */
#define VAB_ID "pBAV"
#define VAB_PROGRAMS_OFFSET 0x12
#define VAB_MASTER_VOLUME_OFFSET 0x18
#define VAB_PROGRAM_TABLE 0x20
#define VAB_PROGRAM_COUNT 128
#define VAB_PROGRAM_BYTES 0x10
#define VAB_TONES_PER_PROGRAM 16
#define VAB_TONE_BYTES 0x20
#define VAB_TONE_BLOCK_BYTES (VAB_TONES_PER_PROGRAM * VAB_TONE_BYTES)
#define VAB_VAG_COUNT 256
#define VAG_SIZE_SHIFT 3
#define PROGRAM_TONES 0x0
#define PROGRAM_VOLUME 0x1
#define PROGRAM_PAN 0x4
#define TONE_MODE 0x1
#define TONE_VOLUME 0x2
#define TONE_PAN 0x3
#define TONE_CENTER 0x4
#define TONE_SHIFT 0x5
#define TONE_MIN 0x6
#define TONE_MAX 0x7
#define TONE_BEND_MIN 0xC
#define TONE_BEND_MAX 0xD
#define TONE_ADSR1 0x10
#define TONE_ADSR2 0x12
#define TONE_VAG 0x16
#define TONE_MODE_REVERB 0x4

/* SEP's and SEQ's layout: the file's ID and version, then each sequence's ID, resolution, tempo, rhythm, data size */
#define SEP_HEADER_BYTES 6
#define SEQ_ID_BYTES 2
#define SEQ_RESOLUTION_BYTES 2
#define SEQ_TEMPO_BYTES 3
#define SEQ_RHYTHM_BYTES 2
#define SEQ_SIZE_BYTES 4
#define SEQ_HEADER_BYTES (SEQ_ID_BYTES + SEQ_RESOLUTION_BYTES + SEQ_TEMPO_BYTES + SEQ_RHYTHM_BYTES + SEQ_SIZE_BYTES)

/* MIDI */
#define STATUS_BIT 0x80
#define STATUS_KIND 0xF0
#define STATUS_CHANNEL 0x0F
#define NOTE_OFF 0x80
#define NOTE_ON 0x90
#define CONTROL 0xB0
#define PROGRAM 0xC0
#define PITCH_BEND 0xE0
#define META 0xFF
#define META_TEMPO 0x51
#define META_END 0x2F
#define CONTROL_DATA_ENTRY 6
#define CONTROL_VOLUME 7
#define CONTROL_PAN 10
#define CONTROL_EXPRESSION 11
#define CONTROL_NRPN_MSB 99
/* libsnd's repeats, by NRPN: a loop's start (its count in the data entry) and its end */
#define NRPN_LOOP_START 20
#define NRPN_LOOP_END 30
/* A note's or controller's two data bytes */
#define EVENT_DATA_BYTES 2
#define VARIABLE_LENGTH_MORE 0x80
#define VARIABLE_LENGTH_BITS 7
#define SEVEN_BITS 0x7F
#define BYTE_BITS 8
#define CENTER_PAN 64
#define MIDI_MAX 127
#define BEND_CENTER 0x2000
#define BEND_SHIFT 7

/* The SPU's voice registers, and its volumes and pitch */
#define SPU_VOICE_BASE 0x1F801C00u
#define SPU_VOICE_STRIDE 0x10
#define VOICE_VOLUME_LEFT 0x0
#define VOICE_VOLUME_RIGHT 0x2
#define VOICE_PITCH 0x4
#define VOICE_START 0x6
#define VOICE_ADSR_LOW 0x8
#define VOICE_ADSR_HIGH 0xA
#define VOICE_ENVELOPE 0xC
#define SPU_MAIN_VOLUME_LEFT 0x1F801D80u
#define SPU_MAIN_VOLUME_RIGHT 0x1F801D82u
#define ADDRESS_SHIFT 3
#define VOLUME_MAX 0x3FFF
#define PITCH_ONE 0x1000
#define PITCH_MAX 0x3FFF
#define SEMITONES 12.0
#define FINE_STEPS 128.0
/* SsSetMVol's 127 is the SPU's 0x3FFF */
#define MASTER_SCALE 129

/* Ticks at 60 a second (SsSetTickMode 1); a tempo in microseconds a quarter note */
#define TICKS_PER_SECOND 60.0
#define MICROSECONDS 1000000.0
#define ACCESS_FAILED -1
#define LOOP_FOREVER 0

enum {
    SEQ_STOPPED = 0,
    SEQ_PLAYING = 1,
    SEQ_PAUSED = 2
};

typedef struct {
    const uint8_t *header;
    uint32_t vag_address[VAB_VAG_COUNT];
    int16_t tone_block[VAB_PROGRAM_COUNT];
    int open;
} Vab;

typedef struct {
    int program;
    int volume, pan, expression;
    int bend;
    int nrpn;
} Channel;

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const uint8_t *position;
    const uint8_t *loop_start;
    int loop_count;
    int resolution;
    uint32_t tempo;
    double delta;
    uint8_t running;
    int state;
    int plays_left;
    int volume_left, volume_right;
    /* A crescendo's or decrescendo's step a tick and the ticks it goes on for */
    double ramp_step;
    double ramp_volume;
    int ramp_ticks;
    Channel channels[CHANNELS];
} Sequence;

typedef struct {
    int vab;
    int count;
    Sequence sequences[SEQ_MAX];
    int open;
} Sep;

typedef struct {
    int sep, seq, channel, note;
    const uint8_t *tone;
    const uint8_t *program;
    int velocity;
    uint32_t age;
} Voice;

static Vab s_Vabs[VAB_MAX];
static Sep s_Seps[SEP_MAX];
static Voice s_Voices[VOICES];
static int s_ReservedVoices = VOICES;
static int s_Ticking;
static uint32_t s_Age;

void func_80051078(int on, uint32_t voices);
uint32_t SpuSetReverbVoice(int on, uint32_t voices);

static uint32_t big_endian(const uint8_t *bytes, int count) {
    uint32_t value = 0;
    int index;

    for (index = 0; index < count; index++) {
        value = (value << BYTE_BITS) | bytes[index];
    }
    return value;
}

static uint16_t little_half(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | (bytes[1] << BYTE_BITS));
}

static uint32_t voice_register(int voice, uint32_t offset) {
    return SPU_VOICE_BASE + (uint32_t)voice * SPU_VOICE_STRIDE + offset;
}

void SsInit(void) {
    int voice;

    Host_SpuStart();
    for (voice = 0; voice < VOICES; voice++) {
        s_Voices[voice].note = NOTE_NONE;
    }
}

void SsSetTableSize(void *table, int seqs, int seps) {
    (void)table;
    (void)seqs;
    (void)seps;
}

void SsSetTickMode(int mode) {
    (void)mode;
}

void SsStart(void) {
    s_Ticking = 1;
}

void SsEnd(void) {
    s_Ticking = 0;
}

void SsSetReservedVoice(unsigned char voices) {
    s_ReservedVoices = voices < VOICES ? voices : VOICES;
}

void SsSetMVol(int left, int right) {
    Host_SpuStart();
    Spu_WriteRegister(SPU_MAIN_VOLUME_LEFT, (uint16_t)(left * MASTER_SCALE));
    Spu_WriteRegister(SPU_MAIN_VOLUME_RIGHT, (uint16_t)(right * MASTER_SCALE));
}

/* A VAB's header, its samples already in the SPU's RAM from sample_base: each VAG's address from the header's sizes,
 * each program's tones by the order of the programs that have some */
int16_t SsVabFakeHead(const uint8_t *header, int16_t vab_asked, uint32_t sample_base) {
    int vab = vab_asked;
    Vab *slot;
    const uint8_t *sizes;
    int programs;
    int index;
    int block = 0;
    uint32_t address = sample_base;

    if (vab < 0) {
        for (vab = 0; vab < VAB_MAX && s_Vabs[vab].open; vab++) {
            /* The first free slot */
        }
    } else {
        /* The slot asked for */
    }
    if (vab >= VAB_MAX || memcmp(header, VAB_ID, strlen(VAB_ID)) != 0) {
        Host_Log("SsVabFakeHead: no slot, or not a VAB");
        return ACCESS_FAILED;
    } else {
        slot = &s_Vabs[vab];
        programs = little_half(header + VAB_PROGRAMS_OFFSET);
    }
    for (index = 0; index < VAB_PROGRAM_COUNT; index++) {
        if (header[VAB_PROGRAM_TABLE + index * VAB_PROGRAM_BYTES + PROGRAM_TONES] != 0 && block < programs) {
            slot->tone_block[index] = (int16_t)block++;
        } else {
            slot->tone_block[index] = NOTE_NONE;
        }
    }
    sizes = header + VAB_PROGRAM_TABLE + VAB_PROGRAM_COUNT * VAB_PROGRAM_BYTES + programs * VAB_TONE_BLOCK_BYTES;
    for (index = 0; index < VAB_VAG_COUNT; index++) {
        slot->vag_address[index] = address;
        address += (uint32_t)little_half(sizes + index * sizeof(uint16_t)) << VAG_SIZE_SHIFT;
    }
    slot->header = header;
    slot->open = 1;
    return (int16_t)vab;
}

int16_t SsVabFakeBody(int16_t vab) {
    return vab;
}

void SsVabClose(int16_t vab) {
    if (vab >= 0 && vab < VAB_MAX) {
        s_Vabs[vab].open = 0;
    } else {
        /* No such VAB */
    }
}

static void rewind_sequence(Sequence *seq) {
    int channel;

    seq->position = seq->start;
    seq->loop_start = NULL;
    seq->loop_count = 0;
    seq->delta = 0;
    seq->running = 0;
    for (channel = 0; channel < CHANNELS; channel++) {
        seq->channels[channel].program = channel;
        seq->channels[channel].volume = MIDI_MAX;
        seq->channels[channel].pan = CENTER_PAN;
        seq->channels[channel].expression = MIDI_MAX;
        seq->channels[channel].bend = BEND_CENTER;
        seq->channels[channel].nrpn = 0;
    }
}

static uint32_t read_variable(const uint8_t **position) {
    uint32_t value = 0;
    uint8_t byte;

    do {
        byte = *(*position)++;
        value = (value << VARIABLE_LENGTH_BITS) | (byte & SEVEN_BITS);
    } while (byte & VARIABLE_LENGTH_MORE);
    return value;
}

/* A SEP's sequences: each one's header, its score after it, the first event's delta read */
int16_t SsSepOpen(const uint8_t *sep, int16_t vab, int16_t count) {
    int access;
    const uint8_t *position = sep + SEP_HEADER_BYTES;
    int index;

    for (access = 0; access < SEP_MAX && s_Seps[access].open; access++) {
        /* The first free access */
    }
    if (access >= SEP_MAX || count > SEQ_MAX) {
        Host_Log("SsSepOpen: no access left for %d sequences", count);
        return ACCESS_FAILED;
    } else {
        s_Seps[access].vab = vab;
        s_Seps[access].count = count;
        s_Seps[access].open = 1;
    }
    for (index = 0; index < count; index++) {
        Sequence *seq = &s_Seps[access].sequences[index];
        uint32_t size;

        seq->resolution = (int)big_endian(position + SEQ_ID_BYTES, SEQ_RESOLUTION_BYTES);
        seq->tempo = big_endian(position + SEQ_ID_BYTES + SEQ_RESOLUTION_BYTES, SEQ_TEMPO_BYTES);
        size = big_endian(position + SEQ_HEADER_BYTES - SEQ_SIZE_BYTES, SEQ_SIZE_BYTES);
        seq->start = position + SEQ_HEADER_BYTES;
        seq->end = seq->start + size;
        seq->state = SEQ_STOPPED;
        seq->volume_left = MIDI_MAX;
        seq->volume_right = MIDI_MAX;
        seq->ramp_ticks = 0;
        rewind_sequence(seq);
        position = seq->end;
    }
    return (int16_t)access;
}

static Sequence *sequence(int access, int seq) {
    if (access < 0 || access >= SEP_MAX || !s_Seps[access].open || seq < 0 || seq >= s_Seps[access].count) {
        return NULL;
    } else {
        return &s_Seps[access].sequences[seq];
    }
}

/* The voices a sequence sounds keyed off, freed */
static void silence(int access, int seq) {
    uint32_t voices = 0;
    int voice;

    for (voice = 0; voice < VOICES; voice++) {
        if (s_Voices[voice].note != NOTE_NONE && s_Voices[voice].sep == access && s_Voices[voice].seq == seq) {
            voices |= 1u << voice;
            s_Voices[voice].note = NOTE_NONE;
        } else {
            /* Another's */
        }
    }
    if (voices != 0) {
        func_80051078(0, voices);
    } else {
        /* None sounding */
    }
}

void SsSepClose(int16_t access) {
    int seq;

    if (access >= 0 && access < SEP_MAX && s_Seps[access].open) {
        for (seq = 0; seq < s_Seps[access].count; seq++) {
            silence(access, seq);
        }
        s_Seps[access].open = 0;
    } else {
        /* No such SEP */
    }
}

/* SsSepPlay: from the start, l_count times through (0 for ever) */
void SsSepPlay(int16_t access, int16_t seq, char mode, int16_t plays) {
    Sequence *s = sequence(access, seq);

    (void)mode;
    if (s == NULL) {
        return;
    } else {
        silence(access, seq);
        rewind_sequence(s);
        s->delta = read_variable(&s->position);
        s->plays_left = plays;
        s->state = SEQ_PLAYING;
    }
}

/* SsSepStop */
void func_80056304(int16_t access, int16_t seq) {
    Sequence *s = sequence(access, seq);

    if (s == NULL) {
        return;
    } else {
        silence(access, seq);
        s->state = SEQ_STOPPED;
    }
}

void SsSepPause(int16_t access, int16_t seq) {
    Sequence *s = sequence(access, seq);

    if (s == NULL || s->state != SEQ_PLAYING) {
        return;
    } else {
        silence(access, seq);
        s->state = SEQ_PAUSED;
    }
}

void SsSepReplay(int16_t access, int16_t seq) {
    Sequence *s = sequence(access, seq);

    if (s == NULL || s->state != SEQ_PAUSED) {
        return;
    } else {
        s->state = SEQ_PLAYING;
    }
}

static int clamp(int value, int low, int high) {
    if (value < low) {
        return low;
    } else if (value > high) {
        return high;
    } else {
        return value;
    }
}

/* A voice's volumes: the note's velocity, its channel's volume and expression, its tone's, program's and VAB's, its
 * sequence's, each of 127; panned by the tone's, program's and channel's pans around the centre */
static void voice_volume(int voice) {
    const Voice *v = &s_Voices[voice];
    const Sequence *seq = &s_Seps[v->sep].sequences[v->seq];
    const Channel *channel = &seq->channels[v->channel];
    const uint8_t *vab = s_Vabs[s_Seps[v->sep].vab].header;
    double level = (double)v->velocity / MIDI_MAX * channel->volume / MIDI_MAX * channel->expression / MIDI_MAX *
                   v->tone[TONE_VOLUME] / MIDI_MAX * v->program[PROGRAM_VOLUME] / MIDI_MAX *
                   vab[VAB_MASTER_VOLUME_OFFSET] / MIDI_MAX * VOLUME_MAX;
    int pan = clamp(v->tone[TONE_PAN] + v->program[PROGRAM_PAN] + channel->pan - CENTER_PAN - CENTER_PAN, 0, MIDI_MAX);
    double left = level * seq->volume_left / MIDI_MAX;
    double right = level * seq->volume_right / MIDI_MAX;

    if (pan > CENTER_PAN) {
        left = left * (MIDI_MAX - pan) / (MIDI_MAX - CENTER_PAN);
    } else if (pan < CENTER_PAN) {
        right = right * pan / CENTER_PAN;
    } else {
        /* Centred */
    }
    Spu_WriteRegister(voice_register(voice, VOICE_VOLUME_LEFT), (uint16_t)clamp((int)left, 0, VOLUME_MAX));
    Spu_WriteRegister(voice_register(voice, VOICE_VOLUME_RIGHT), (uint16_t)clamp((int)right, 0, VOLUME_MAX));
}

/* A voice's pitch: the note against the tone's centre and fine tuning, bent within the tone's range */
static void voice_pitch(int voice) {
    const Voice *v = &s_Voices[voice];
    const Channel *channel = &s_Seps[v->sep].sequences[v->seq].channels[v->channel];
    int bend = channel->bend - BEND_CENTER;
    double range = bend >= 0 ? v->tone[TONE_BEND_MAX] : v->tone[TONE_BEND_MIN];
    double semitones = v->note - v->tone[TONE_CENTER] + v->tone[TONE_SHIFT] / FINE_STEPS +
                       range * bend / BEND_CENTER;
    double pitch = PITCH_ONE * pow(2.0, semitones / SEMITONES);

    Spu_WriteRegister(voice_register(voice, VOICE_PITCH), (uint16_t)clamp((int)pitch, 0, PITCH_MAX));
}

/* A free voice among libsnd's, else the one sounding longest */
static int allocate_voice(void) {
    int voice;
    int oldest = 0;

    for (voice = 0; voice < s_ReservedVoices; voice++) {
        if (s_Voices[voice].note == NOTE_NONE &&
            Spu_ReadRegister(voice_register(voice, VOICE_ENVELOPE)) == 0) {
            return voice;
        } else if (s_Voices[voice].age < s_Voices[oldest].age) {
            oldest = voice;
        } else {
            /* Younger */
        }
    }
    return s_ReservedVoices > 0 ? oldest : NOTE_NONE;
}

static void note_on(int access, int seq, int channel, int note, int velocity) {
    Sep *sep = &s_Seps[access];
    const Vab *vab = &s_Vabs[sep->vab];
    int program = sep->sequences[seq].channels[channel].program;
    const uint8_t *program_attributes = vab->header + VAB_PROGRAM_TABLE + program * VAB_PROGRAM_BYTES;
    int tones = program_attributes[PROGRAM_TONES];
    int block = vab->tone_block[program];
    int index;

    if (!vab->open || block < 0) {
        return;
    } else {
        /* Each of the program's tones that spans the note sounds */
    }
    for (index = 0; index < tones && index < VAB_TONES_PER_PROGRAM; index++) {
        const uint8_t *tone = vab->header + VAB_PROGRAM_TABLE + VAB_PROGRAM_COUNT * VAB_PROGRAM_BYTES +
                              block * VAB_TONE_BLOCK_BYTES + index * VAB_TONE_BYTES;
        int voice;

        if (note < tone[TONE_MIN] || note > tone[TONE_MAX]) {
            continue;
        } else {
            voice = allocate_voice();
        }
        if (voice == NOTE_NONE) {
            return;
        } else {
            s_Voices[voice].sep = access;
            s_Voices[voice].seq = seq;
            s_Voices[voice].channel = channel;
            s_Voices[voice].note = note;
            s_Voices[voice].tone = tone;
            s_Voices[voice].program = program_attributes;
            s_Voices[voice].velocity = velocity;
            s_Voices[voice].age = ++s_Age;
        }
        voice_volume(voice);
        voice_pitch(voice);
        Spu_WriteRegister(voice_register(voice, VOICE_START),
                          (uint16_t)(vab->vag_address[little_half(tone + TONE_VAG)] >> ADDRESS_SHIFT));
        Spu_WriteRegister(voice_register(voice, VOICE_ADSR_LOW), little_half(tone + TONE_ADSR1));
        Spu_WriteRegister(voice_register(voice, VOICE_ADSR_HIGH), little_half(tone + TONE_ADSR2));
        SpuSetReverbVoice((tone[TONE_MODE] & TONE_MODE_REVERB) != 0, 1u << voice);
        func_80051078(1, 1u << voice);
    }
}

static void note_off(int access, int seq, int channel, int note) {
    int voice;

    for (voice = 0; voice < VOICES; voice++) {
        Voice *v = &s_Voices[voice];

        if (v->note == note && v->sep == access && v->seq == seq && v->channel == channel) {
            v->note = NOTE_NONE;
            func_80051078(0, 1u << voice);
        } else {
            /* Another note */
        }
    }
}

/* The sounding voices of a sequence's channel (or all its channels, CHANNELS) given their new volume or pitch */
static void update_voices(int access, int seq, int channel, int pitch) {
    int voice;

    for (voice = 0; voice < VOICES; voice++) {
        const Voice *v = &s_Voices[voice];

        if (v->note == NOTE_NONE || v->sep != access || v->seq != seq ||
            (channel != CHANNELS && v->channel != channel)) {
            continue;
        } else if (pitch) {
            voice_pitch(voice);
        } else {
            voice_volume(voice);
        }
    }
}

static void control(int access, int seq, int channel, int number, int value) {
    Sequence *s = &s_Seps[access].sequences[seq];
    Channel *c = &s->channels[channel];

    switch (number) {
    case CONTROL_VOLUME:
        c->volume = value;
        update_voices(access, seq, channel, 0);
        break;
    case CONTROL_PAN:
        c->pan = value;
        update_voices(access, seq, channel, 0);
        break;
    case CONTROL_EXPRESSION:
        c->expression = value;
        update_voices(access, seq, channel, 0);
        break;
    case CONTROL_NRPN_MSB:
        c->nrpn = value;
        if (value == NRPN_LOOP_END && s->loop_start != NULL && s->loop_count != 1) {
            s->loop_count = s->loop_count > 1 ? s->loop_count - 1 : s->loop_count;
            s->position = s->loop_start;
        } else if (value == NRPN_LOOP_START) {
            s->loop_start = s->position;
        } else {
            /* Another parameter */
        }
        break;
    case CONTROL_DATA_ENTRY:
        if (c->nrpn == NRPN_LOOP_START) {
            s->loop_count = value;
        } else {
            /* Data for another parameter */
        }
        break;
    default:
        /* Not one libsnd's sequences use for sound */
        break;
    }
}

/* The sequence's end: again from the start while plays are left (or for ever), else stopped */
static void end_of_sequence(int access, int seq) {
    Sequence *s = &s_Seps[access].sequences[seq];

    if (s->plays_left == LOOP_FOREVER || --s->plays_left > 0) {
        rewind_sequence(s);
        s->delta = read_variable(&s->position);
    } else {
        silence(access, seq);
        s->state = SEQ_STOPPED;
    }
}

/* The events due now, until the next delta that isn't 0 */
static void run_events(int access, int seq) {
    Sequence *s = &s_Seps[access].sequences[seq];

    while (s->state == SEQ_PLAYING && s->delta <= 0) {
        uint8_t status;
        int channel;

        if (s->position >= s->end) {
            end_of_sequence(access, seq);
            continue;
        } else if (*s->position & STATUS_BIT) {
            status = *s->position++;
            s->running = status;
        } else {
            status = s->running;
        }
        channel = status & STATUS_CHANNEL;
        if (status == META) {
            uint8_t type = *s->position++;
            uint32_t length = read_variable(&s->position);

            if (type == META_TEMPO) {
                s->tempo = big_endian(s->position, SEQ_TEMPO_BYTES);
            } else if (type == META_END) {
                s->position = s->end;
            } else {
                /* Another meta event */
            }
            s->position += type == META_END ? 0 : length;
        } else if ((status & STATUS_KIND) == NOTE_ON && s->position[1] != 0) {
            note_on(access, seq, channel, s->position[0], s->position[1]);
            s->position += EVENT_DATA_BYTES;
        } else if ((status & STATUS_KIND) == NOTE_ON || (status & STATUS_KIND) == NOTE_OFF) {
            note_off(access, seq, channel, s->position[0]);
            s->position += EVENT_DATA_BYTES;
        } else if ((status & STATUS_KIND) == CONTROL) {
            control(access, seq, channel, s->position[0], s->position[1]);
            s->position += EVENT_DATA_BYTES;
        } else if ((status & STATUS_KIND) == PROGRAM) {
            s->channels[channel].program = *s->position++;
        } else if ((status & STATUS_KIND) == PITCH_BEND) {
            s->channels[channel].bend = s->position[0] | (s->position[1] << BEND_SHIFT);
            s->position += EVENT_DATA_BYTES;
            update_voices(access, seq, channel, 1);
        } else {
            Host_Log("libsnd: status 0x%02X not played", status);
            s->position = s->end;
        }
        if (s->state == SEQ_PLAYING && s->position < s->end) {
            s->delta += read_variable(&s->position);
        } else {
            /* Ended, or stopped */
        }
    }
}

static void ramp(int access, int seq) {
    Sequence *s = &s_Seps[access].sequences[seq];

    if (s->ramp_ticks <= 0) {
        return;
    } else {
        s->ramp_volume += s->ramp_step;
        s->ramp_ticks--;
        s->volume_left = clamp((int)s->ramp_volume, 0, MIDI_MAX);
        s->volume_right = s->volume_left;
        update_voices(access, seq, CHANNELS, 0);
    }
}

/* SsSeqCalledTbyT, the tick libsnd's data names: each playing sequence on by a 60th of a second of its music */
void SsSeqCalledTbyT(void) {
    int access, seq;

    if (!s_Ticking) {
        return;
    } else {
        /* Every open sequence */
    }
    for (access = 0; access < SEP_MAX; access++) {
        for (seq = 0; s_Seps[access].open && seq < s_Seps[access].count; seq++) {
            Sequence *s = &s_Seps[access].sequences[seq];

            ramp(access, seq);
            if (s->state == SEQ_PLAYING) {
                s->delta -= s->resolution * MICROSECONDS / s->tempo / TICKS_PER_SECOND;
                run_events(access, seq);
            } else {
                /* Stopped or paused */
            }
        }
    }
}

/* SsSepSetVol */
void func_80056760(int16_t access, int16_t seq, int16_t left, int16_t right) {
    Sequence *s = sequence(access, seq);

    if (s == NULL) {
        return;
    } else {
        s->volume_left = left;
        s->volume_right = right;
        s->ramp_ticks = 0;
        update_voices(access, seq, CHANNELS, 0);
    }
}

static void set_ramp(int access, int seq, int change, int ticks) {
    Sequence *s = sequence(access, seq);

    if (s == NULL || ticks <= 0) {
        return;
    } else {
        s->ramp_volume = s->volume_left;
        s->ramp_step = (double)change / ticks;
        s->ramp_ticks = ticks;
    }
}

/* SsSepSetCrescendo: the volume up by vol over ticks */
void func_800528C4(int16_t access, int16_t seq, int vol, int ticks) {
    set_ramp(access, seq, vol, ticks);
}

/* SsSepSetDecrescendo: the volume down by vol over ticks */
void func_80052B4C(int16_t access, int16_t seq, int vol, int ticks) {
    set_ramp(access, seq, -vol, ticks);
}
