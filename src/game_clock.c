#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_clock.h"
#include "gpu.h"
#include "host_log.h"
#include "input.h"
#include "psyq/host_psyq.h"
#include "retail.h"

/* With CRASH2_GAME_CLOCK=DIR, the game clock of decomp's tools/run_game.lua: the root counter's interrupts don't reach
 * the game, and on entering Gfx_EndFrame (FRAME_ENDING) g_TimerTicks becomes the frame's number times 34, every game
 * frame lasting two vertical blanks for the game. Each entry writes a line to DIR/game_frames.txt as run_game.lua
 * does: the game frame displayed, the vertical blank it was shown at, and a 64-bit FNV-1a over the displayed
 * rectangle's 16-bit pixels taken as 32-bit words. CRASH2_GAME_CLOCK_LAST=N ends the run after game frame N;
 * CRASH2_GAME_CLOCK_SHOTS=N,M,... writes those game frames' images as DIR/shot_N.ppm, run_game.lua's shots, and
 * CRASH2_GAME_CLOCK_DUMPS=N,M,... the game's data and bss as DIR/ram_N.bin, a 2 MB image of the RAM at the retail
 * addresses (the rest zero), to compare with run_game.lua's RAM dumps */

#define CLOCK_VARIABLE "CRASH2_GAME_CLOCK"
#define LAST_VARIABLE "CRASH2_GAME_CLOCK_LAST"
#define SHOTS_VARIABLE "CRASH2_GAME_CLOCK_SHOTS"
#define SHOTS_MAX 64
#define DUMPS_VARIABLE "CRASH2_GAME_CLOCK_DUMPS"
/* CRASH2_GAME_CLOCK_OBJECTS=N,M,...: the game's live objects at those game frames as DIR/objects_N.txt, a line each:
 * its address, its GOOL entry's name, subtype, state, the code word its pc is at, display mode and statuses */
#define OBJECTS_VARIABLE "CRASH2_GAME_CLOCK_OBJECTS"
/* A GoolObject's fields (decomp's include/game/object.h) and its GOOL entry's (include/game/nsf.h): the entry, its
 * EID, its code (item 1) */
#define OBJECT_EXE 0x0C
#define OBJECT_STATE 0x1C
#define OBJECT_STATUS_A 0xA8
#define OBJECT_STATUS_B 0xAC
#define OBJECT_SUBTYPE 0xB4
#define OBJECT_PC 0xC0
#define OBJECT_DISPLAY_MODE 0x108
#define ENTRY_EID 0x04
#define ENTRY_CODE_ITEM 0x14
/* An EID's name: 5 characters of 6 bits from bit 25 down, in the game's alphabet */
#define EID_NAME_CHARACTERS 5
#define EID_FIRST_SHIFT 25
#define EID_CHARACTER_BITS 6
#define EID_CHARACTER_MASK 0x3F
#define WORD_BYTES 4
/* CRASH2_GAME_CLOCK_PRESSES=FILE: run_game.lua's schedule (decomp's build/run_game/NAME/schedule.txt), whose "press
 * FIRST LAST BUTTON" lines hold BUTTON from game frame FIRST to LAST, its other lines left to run_game.lua */
#define PRESSES_VARIABLE "CRASH2_GAME_CLOCK_PRESSES"
#define PRESSES_MAX 256
#define LINE_BYTES 256
#define PRESS_COMMAND "press "
/* A button's name, at most BUTTON_NAME_BYTES with its end, and the scan of a press line's fields after the command */
#define BUTTON_NAME_BYTES 16
#define PRESS_FORMAT "%d %d %15s"
#define PRESS_FIELDS 3
/* The PlayStation's 2 MB, which holds the retail image's data and bss (retail.h) */
#define RAM_BYTES 0x200000
#define RAM_ADDRESS_MASK (RAM_BYTES - 1)
#define FIVE_BITS 31
#define GREEN_SHIFT 5
#define BLUE_SHIFT 10
#define CHANNEL_SHIFT 3
#define DECIMAL 10
#define GAME_FRAME_TICKS 34
#define PATH_BYTES 4096
#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL
#define HALF_SHIFT 16

extern unsigned int g_TimerTicks;

static int s_Checked;
static FILE *s_Frames;
static int s_GameFrame;
static int s_LastFrame = -1;
static const char *s_Directory;
static int s_Shots[SHOTS_MAX];
static int s_ShotCount;
static int s_Dumps[SHOTS_MAX];
static int s_DumpCount;
static int s_Objects[SHOTS_MAX];
static int s_ObjectCount;

typedef struct {
    int first, last;
    unsigned int button;
} Press;

static Press s_Presses[PRESSES_MAX];
static int s_PressCount;
static unsigned int s_Held;

/* PCSX.CONSTS.PAD.BUTTON's names, which run_game.lua's schedules use */
static const struct {
    const char *name;
    unsigned int button;
} s_ButtonNames[] = {
    {"SELECT", PAD_SELECT}, {"L3", PAD_L3},     {"R3", PAD_R3},     {"START", PAD_START},
    {"UP", PAD_UP},         {"RIGHT", PAD_RIGHT}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT},
    {"L2", PAD_L2},         {"R2", PAD_R2},     {"L1", PAD_L1},     {"R1", PAD_R1},
    {"TRIANGLE", PAD_TRIANGLE}, {"CIRCLE", PAD_CIRCLE}, {"CROSS", PAD_CROSS}, {"SQUARE", PAD_SQUARE},
};

static unsigned int button_named(const char *name) {
    size_t index;

    for (index = 0; index < sizeof(s_ButtonNames) / sizeof(s_ButtonNames[0]); index++) {
        if (strcmp(s_ButtonNames[index].name, name) == 0) {
            return s_ButtonNames[index].button;
        } else {
            continue;
        }
    }
    Host_Fatal("%s: no button %s", PRESSES_VARIABLE, name);
    return 0;
}

static void read_presses(const char *path) {
    FILE *file = fopen(path, "r");
    char line[LINE_BYTES];

    if (file == NULL) {
        Host_Fatal("%s: can't read %s", PRESSES_VARIABLE, path);
    } else {
        while (fgets(line, sizeof(line), file) != NULL) {
            char name[BUTTON_NAME_BYTES];
            Press press;

            if (strncmp(line, PRESS_COMMAND, strlen(PRESS_COMMAND)) != 0) {
                continue;
            } else if (sscanf(line + strlen(PRESS_COMMAND), PRESS_FORMAT, &press.first, &press.last, name) !=
                       PRESS_FIELDS || s_PressCount >= PRESSES_MAX) {
                Host_Fatal("%s: a press line it can't take: %s", PRESSES_VARIABLE, line);
            } else {
                press.button = button_named(name);
                s_Presses[s_PressCount++] = press;
            }
        }
        fclose(file);
    }
}

/* The buttons the schedule holds in a game frame */
static unsigned int held_in(int frame) {
    unsigned int buttons = 0;
    int index;

    for (index = 0; index < s_PressCount; index++) {
        if (frame >= s_Presses[index].first && frame <= s_Presses[index].last) {
            buttons |= s_Presses[index].button;
        } else {
            /* Not this press's frames */
        }
    }
    return buttons;
}

unsigned int Host_ClockButtons(void) {
    return s_Held;
}

/* The split's first data symbol: the retail data's start, where tools/check_layout.py keeps the rest after it */

/* A comma-separated list of game frames into frames (SHOTS_MAX at most): their number */
static int frame_list(const char *text, int *frames) {
    int count = 0;

    while (text != NULL && *text != '\0' && count < SHOTS_MAX) {
        char *end;

        frames[count++] = (int)strtol(text, &end, DECIMAL);
        text = *end == ',' ? end + 1 : end;
    }
    return count;
}

static void start(void) {
    const char *directory = getenv(CLOCK_VARIABLE);
    const char *last = getenv(LAST_VARIABLE);
    char path[PATH_BYTES];

    s_Checked = 1;
    if (directory == NULL) {
        return;
    } else {
        s_Directory = directory;
        snprintf(path, sizeof(path), "%s/game_frames.txt", directory);
        s_Frames = fopen(path, "wb");
    }
    s_ShotCount = frame_list(getenv(SHOTS_VARIABLE), s_Shots);
    s_DumpCount = frame_list(getenv(DUMPS_VARIABLE), s_Dumps);
    s_ObjectCount = frame_list(getenv(OBJECTS_VARIABLE), s_Objects);
    if (getenv(PRESSES_VARIABLE) != NULL) {
        read_presses(getenv(PRESSES_VARIABLE));
    } else {
        /* No presses */
    }
    if (s_Frames == NULL) {
        Host_Fatal("can't write %s", path);
    } else if (last != NULL) {
        s_LastFrame = atoi(last);
    } else {
        /* No last frame: the run goes on */
    }
}

int Host_GameClock(void) {
    if (!s_Checked) {
        start();
    } else {
        /* Checked */
    }
    return s_Frames != NULL;
}

/* The displayed rectangle's pixels, two to a word, the left one in the low half */
static uint64_t display_hash(void) {
    int x, y, width, height, enabled;
    uint64_t hash = FNV_OFFSET;
    int row, column;

    Host_DisplayArea(&x, &y, &width, &height, &enabled);
    for (row = 0; row < height; row++) {
        for (column = 0; column + 1 < width; column += 2) {
            uint32_t low = g_Vram[(y + row) & (VRAM_HEIGHT - 1)][(x + column) & (VRAM_WIDTH - 1)];
            uint32_t high = g_Vram[(y + row) & (VRAM_HEIGHT - 1)][(x + column + 1) & (VRAM_WIDTH - 1)];

            hash = (hash ^ (low | (high << HALF_SHIFT))) * FNV_PRIME;
        }
    }
    return hash;
}

/* The displayed rectangle as a PPM, each 5-bit channel shifted up by 3 as run_game.py converts the emulator's */
static void shot(void) {
    int x, y, width, height, enabled;
    char path[PATH_BYTES];
    FILE *file;
    int row, column;

    snprintf(path, sizeof(path), "%s/shot_%d.ppm", s_Directory, s_GameFrame);
    file = fopen(path, "wb");
    if (file == NULL) {
        Host_Fatal("can't write %s", path);
    } else {
        Host_DisplayArea(&x, &y, &width, &height, &enabled);
        fprintf(file, "P6\n%d %d\n255\n", width, height);
    }
    for (row = 0; row < height; row++) {
        for (column = 0; column < width; column++) {
            uint16_t pixel = g_Vram[(y + row) & (VRAM_HEIGHT - 1)][(x + column) & (VRAM_WIDTH - 1)];
            unsigned char rgb[3];

            rgb[0] = (unsigned char)((pixel & FIVE_BITS) << CHANNEL_SHIFT);
            rgb[1] = (unsigned char)(((pixel >> GREEN_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT);
            rgb[2] = (unsigned char)(((pixel >> BLUE_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT);
            fwrite(rgb, sizeof(rgb), 1, file);
        }
    }
    fclose(file);
}

static int listed(const int *frames, int count, int frame) {
    int index;

    for (index = 0; index < count; index++) {
        if (frames[index] == frame) {
            return 1;
        } else {
            /* Another shot */
        }
    }
    return 0;
}

/* The game's data and bss at their retail addresses in a 2 MB image of the RAM */
static void dump(void) {
    /* Allocated, out of the game's memory window */
    unsigned char *ram = calloc(RAM_BYTES, 1);
    char path[PATH_BYTES];
    FILE *file;

    if (ram == NULL) {
        Host_Fatal("no memory for a RAM dump");
    } else {
        memcpy(ram + (RETAIL_DATA_START & RAM_ADDRESS_MASK), g_EidAlphabet, RETAIL_BSS_END - RETAIL_DATA_START);
        snprintf(path, sizeof(path), "%s/ram_%d.bin", s_Directory, s_GameFrame);
        file = fopen(path, "wb");
    }
    if (file == NULL || fwrite(ram, RAM_BYTES, 1, file) != 1) {
        Host_Fatal("can't write %s", path);
    } else {
        fclose(file);
        free(ram);
    }
}

/* The game's walk over its object lists (decomp's 800158E0.c) */
void Obj_ForEachList(void *each_list, void *each_object, void *argument);
int Obj_ForEachDescendant(void *list, void *each_object, void *argument);

static uint32_t field(const void *base, int offset) {
    uint32_t value;

    memcpy(&value, (const char *)base + offset, sizeof(value));
    return value;
}

static int list_object(void *object, FILE *file) {
    const unsigned char *exe = (const unsigned char *)(uintptr_t)field(object, OBJECT_EXE);
    uint32_t eid = exe == NULL ? 0 : field(exe, ENTRY_EID);
    uint32_t code = exe == NULL ? 0 : field(exe, ENTRY_CODE_ITEM);
    char name[EID_NAME_CHARACTERS + 1];
    int index;

    for (index = 0; index < EID_NAME_CHARACTERS; index++) {
        name[index] = (char)g_EidAlphabet[(eid >> (EID_FIRST_SHIFT - index * EID_CHARACTER_BITS)) & EID_CHARACTER_MASK];
    }
    name[EID_NAME_CHARACTERS] = '\0';
    fprintf(file, "%08X %s subtype %u state %u pc %d display %u status %08X %08X\n", (unsigned int)(uintptr_t)object,
            name, field(object, OBJECT_SUBTYPE), field(object, OBJECT_STATE),
            (int)(field(object, OBJECT_PC) - code) / WORD_BYTES, field(object, OBJECT_DISPLAY_MODE),
            field(object, OBJECT_STATUS_A), field(object, OBJECT_STATUS_B));
    return 0;
}

static void list_objects(void) {
    char path[PATH_BYTES];
    FILE *file;

    snprintf(path, sizeof(path), "%s/objects_%d.txt", s_Directory, s_GameFrame);
    file = fopen(path, "wb");
    if (file == NULL) {
        Host_Fatal("can't write %s", path);
    } else {
        Obj_ForEachList((void *)Obj_ForEachDescendant, (void *)list_object, file);
        fclose(file);
    }
}

void Host_FrameEnding(void) {
    if (!Host_GameClock()) {
        return;
    } else {
        fprintf(s_Frames, "%d %d %016llx\n", s_GameFrame, Host_Blanks(), (unsigned long long)display_hash());
    }
    if (listed(s_Shots, s_ShotCount, s_GameFrame)) {
        shot();
    } else {
        /* Not a checkpoint */
    }
    if (listed(s_Dumps, s_DumpCount, s_GameFrame)) {
        dump();
    } else {
        /* Not a dumped frame */
    }
    if (listed(s_Objects, s_ObjectCount, s_GameFrame)) {
        list_objects();
    } else {
        /* Not a listed frame */
    }
    s_GameFrame++;
    g_TimerTicks = (unsigned int)(s_GameFrame * GAME_FRAME_TICKS);
    /* As run_game.lua's: the presses apply to the game frame after the one ending */
    s_Held = held_in(s_GameFrame + 1);
    if (s_LastFrame >= 0 && s_GameFrame > s_LastFrame) {
        fclose(s_Frames);
        Host_Log("game clock: %d game frames written", s_GameFrame);
        exit(EXIT_SUCCESS);
    } else {
        /* More frames to come */
    }
}
