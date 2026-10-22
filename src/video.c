#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "gpu.h"
#include "audio.h"
#include "host_log.h"
#include "input.h"
#include "psyq/host_psyq.h"
#include "settings.h"
#include "video.h"

/* The display: VRAM's displayed rectangle (PutDispEnv) in an SDL3 window at 4:3, one picture a vertical blank, paced
 * to the NTSC frame rate. CRASH2_HEADLESS runs without a window or pacing; CRASH2_DUMP=DIR writes the display every
 * CRASH2_DUMP_EVERY blanks (60 by default) as DIR/blank_N.ppm, all of VRAM with CRASH2_DUMP_VRAM set */

#define HEADLESS_VARIABLE "CRASH2_HEADLESS"
#define DUMP_VARIABLE "CRASH2_DUMP"
#define DUMP_EVERY_VARIABLE "CRASH2_DUMP_EVERY"
#define DUMP_EVERY_DEFAULT 60
#define DUMP_VRAM_VARIABLE "CRASH2_DUMP_VRAM"
#define PATH_BYTES 4096
#define WINDOW_WIDTH 1280
#define WINDOW_HEIGHT 960
/* The picture's 4:3, as a 640 by 480 canvas the window letterboxes */
#define LOGICAL_WIDTH 640
#define LOGICAL_HEIGHT 480
#define MAX_DISPLAY_WIDTH 640
#define MAX_DISPLAY_HEIGHT 512
#define PIXELS_MAX (VRAM_WIDTH * VRAM_HEIGHT)
/* An NTSC frame: 263 scanlines of 3,413 video cycles at 53.693175 MHz (psx-spx, "GPU Timings"), in nanoseconds */
#define FRAME_NS (263ULL * 3413ULL * 1000000000ULL / 53693175ULL)
#define FIVE_BITS 31
#define RED_SHIFT 0
#define GREEN_SHIFT 5
#define BLUE_SHIFT 10
#define CHANNEL_SHIFT 3
#define COLOUR_RED_SHIFT 16
#define COLOUR_GREEN_SHIFT 8

static SDL_Window *s_Window;
static SDL_Renderer *s_Renderer;
static SDL_Texture *s_Texture;
static int s_Started;
static int s_Headless;
static const char *s_DumpDirectory;
static int s_DumpEvery = DUMP_EVERY_DEFAULT;
static int s_Blanks;
static Uint64 s_NextFrame;
static int s_DumpVram;
/* Allocated, out of the game's memory window */
static uint32_t *s_Pixels;

static void start(void) {
    const char *every = getenv(DUMP_EVERY_VARIABLE);

    s_Started = 1;
    s_Pixels = malloc(PIXELS_MAX * sizeof(uint32_t));
    if (s_Pixels == NULL) {
        Host_Fatal("no memory for the display's pixels");
    } else {
        /* Allocated */
    }
    s_Headless = getenv(HEADLESS_VARIABLE) != NULL;
    s_DumpDirectory = getenv(DUMP_VARIABLE);
    s_DumpVram = getenv(DUMP_VRAM_VARIABLE) != NULL;
    if (every != NULL && atoi(every) > 0) {
        s_DumpEvery = atoi(every);
    } else {
        /* The default interval */
    }
    if (s_Headless) {
        return;
    } else if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        Host_Log("no window (%s): running headless", SDL_GetError());
        s_Headless = 1;
    } else if (!SDL_CreateWindowAndRenderer("Crash Bandicoot 2", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_RESIZABLE,
                                            &s_Window, &s_Renderer)) {
        Host_Log("no window (%s): running headless", SDL_GetError());
        s_Headless = 1;
    } else {
        Host_ApplySettings();
        s_NextFrame = SDL_GetTicksNS();
        Host_InputStart();
        Host_AudioStart();
    }
}

static SDL_ScaleMode scale_mode(void) {
    return g_Settings[SETTING_SMOOTH].value ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST;
}

void Host_ApplySettings(void) {
    SDL_RendererLogicalPresentation shape = SDL_LOGICAL_PRESENTATION_LETTERBOX;

    if (s_Window == NULL) {
        return;
    } else if (g_Settings[SETTING_STRETCH].value) {
        shape = SDL_LOGICAL_PRESENTATION_STRETCH;
    } else {
        /* 4:3, letterboxed */
    }
    SDL_SetWindowFullscreen(s_Window, g_Settings[SETTING_FULLSCREEN].value != 0);
    SDL_SetRenderLogicalPresentation(s_Renderer, LOGICAL_WIDTH, LOGICAL_HEIGHT, shape);
    if (s_Texture != NULL) {
        SDL_SetTextureScaleMode(s_Texture, scale_mode());
    } else {
        /* Set when the first picture makes it */
    }
}

/* The display's pixels as 0xRRGGBB, width by height */
static void convert(int x, int y, int width, int height) {
    int row, column;

    for (row = 0; row < height; row++) {
        for (column = 0; column < width; column++) {
            uint16_t pixel = g_Vram[(y + row) & (VRAM_HEIGHT - 1)][(x + column) & (VRAM_WIDTH - 1)];
            uint32_t red = ((pixel >> RED_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT;
            uint32_t green = ((pixel >> GREEN_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT;
            uint32_t blue = ((pixel >> BLUE_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT;

            s_Pixels[row * width + column] = (red << COLOUR_RED_SHIFT) | (green << COLOUR_GREEN_SHIFT) | blue;
        }
    }
}

static void dump(int width, int height) {
    char path[PATH_BYTES];
    FILE *file;
    int index;

    snprintf(path, sizeof(path), "%s/blank_%d.ppm", s_DumpDirectory, s_Blanks);
    file = fopen(path, "wb");
    if (file == NULL) {
        Host_Log("can't write %s", path);
        return;
    } else {
        fprintf(file, "P6\n%d %d\n255\n", width, height);
    }
    for (index = 0; index < width * height; index++) {
        unsigned char rgb[3];

        rgb[0] = (unsigned char)(s_Pixels[index] >> COLOUR_RED_SHIFT);
        rgb[1] = (unsigned char)(s_Pixels[index] >> COLOUR_GREEN_SHIFT);
        rgb[2] = (unsigned char)s_Pixels[index];
        fwrite(rgb, sizeof(rgb), 1, file);
    }
    fclose(file);
}

static void show(int width, int height) {
    SDL_Event event;
    Uint64 now;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            exit(EXIT_SUCCESS);
        } else {
            Host_InputEvent(&event);
        }
    }
    if (s_Texture == NULL || s_Texture->w != width || s_Texture->h != height) {
        if (s_Texture != NULL) {
            SDL_DestroyTexture(s_Texture);
        } else {
            /* The first picture */
        }
        s_Texture = SDL_CreateTexture(s_Renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        SDL_SetTextureScaleMode(s_Texture, scale_mode());
    } else {
        /* The same size */
    }
    SDL_UpdateTexture(s_Texture, NULL, s_Pixels, width * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(s_Renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(s_Renderer);
    SDL_RenderTexture(s_Renderer, s_Texture, NULL, NULL);
    SDL_RenderPresent(s_Renderer);
    s_NextFrame += FRAME_NS;
    now = SDL_GetTicksNS();
    if (now < s_NextFrame) {
        SDL_DelayPrecise(s_NextFrame - now);
    } else {
        /* Late: the next frame counts from now */
        s_NextFrame = now;
    }
}

void Host_PresentFrame(void) {
    int x, y, width, height, enabled;

    if (!s_Started) {
        start();
    } else {
        /* Started */
    }
    s_Blanks++;
    Host_DisplayArea(&x, &y, &width, &height, &enabled);
    if (width <= 0 || height <= 0 || width > MAX_DISPLAY_WIDTH || height > MAX_DISPLAY_HEIGHT) {
        return;
    } else if (!enabled) {
        SDL_memset(s_Pixels, 0, PIXELS_MAX * sizeof(uint32_t));
    } else {
        convert(x, y, width, height);
    }
    if (s_DumpDirectory != NULL && s_Blanks % s_DumpEvery == 0 && s_DumpVram) {
        convert(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
        dump(VRAM_WIDTH, VRAM_HEIGHT);
        convert(x, y, width, height);
    } else if (s_DumpDirectory != NULL && s_Blanks % s_DumpEvery == 0) {
        dump(width, height);
    } else {
        /* Not a dumped blank */
    }
    if (!s_Headless) {
        show(width, height);
    } else {
        /* No window */
    }
}
