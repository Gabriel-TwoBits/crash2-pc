#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_clock.h"
#include "gpu.h"
#include "host_log.h"
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
        s_Frames = fopen(path, "w");
    }
    s_ShotCount = frame_list(getenv(SHOTS_VARIABLE), s_Shots);
    s_DumpCount = frame_list(getenv(DUMPS_VARIABLE), s_Dumps);
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
    s_GameFrame++;
    g_TimerTicks = (unsigned int)(s_GameFrame * GAME_FRAME_TICKS);
    if (s_LastFrame >= 0 && s_GameFrame > s_LastFrame) {
        fclose(s_Frames);
        Host_Log("game clock: %d game frames written", s_GameFrame);
        exit(EXIT_SUCCESS);
    } else {
        /* More frames to come */
    }
}
