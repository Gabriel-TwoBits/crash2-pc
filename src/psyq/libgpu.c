#include <stdint.h>
#include <string.h>

#include "gpu.h"
#include "psyq/host_psyq.h"

/* libgpu on the software GPU (gpu.c): the drawing and display environments, the ordering table's walk, the primitives'
 * helpers and the VRAM transfers the game calls, from their documented behaviour (libgpu's structures as the game's
 * C lays them out: DISPENV's 0x14 bytes, DRAWENV's dtd at 0x16 and dfe at 0x17). Everything completes at once */

typedef struct {
    int16_t x, y, w, h;
} Rect;

typedef struct {
    Rect clip;
    int16_t ofs[2];
    Rect tw;
    uint16_t tpage;
    uint8_t dtd;
    uint8_t dfe;
    uint8_t isbg;
    uint8_t r0, g0, b0;
    uint32_t dr_env[16];
} DrawEnv;

typedef struct {
    Rect disp;
    Rect screen;
    uint8_t isinter;
    uint8_t isrgb24;
    uint8_t pad0, pad1;
} DispEnv;

/* A primitive's tag: its length in words in the top byte, the next's address in the low 24 bits */
#define TAG_LENGTH_SHIFT 24
#define TAG_LINK_MASK 0xFFFFFF
#define TAG_END 0xFFFFFF
#define KSEG0 0x80000000u
#define CODE_BYTE 3
#define SEMI_BIT 0x02
#define POLY_F4_CODE 0x28
#define POLY_F4_LENGTH 5
#define DRAW_MODE_LENGTH 2
#define DRAW_MODE_COMMAND 0xE1000000u
#define TEXTURE_WINDOW_COMMAND 0xE2000000u
#define FILL_COMMAND 0x02000000u
#define DRAW_MODE_DITHER 0x200
#define DRAW_MODE_DISPLAY 0x400
#define DRAW_MODE_PAGE_MASK 0x9FF
/* SetDefDrawEnv's texture page: 640, 0, 4-bit (GetTPage(0, 0, 640, 0)) */
#define DEFAULT_TPAGE 0x0A
#define FILL_X_MASK 0x3F0
#define FILL_WIDTH_ROUND 0xF
#define HALF_SHIFT 16
#define HALF_MASK 0xFFFF
#define COLOUR_GREEN_SHIFT 8
#define COLOUR_BLUE_SHIFT 16
#define WINDOW_UNIT_SHIFT 3
#define WINDOW_FIELD_MASK 0x1F
#define WINDOW_MASK_Y_SHIFT 5
#define WINDOW_OFFSET_X_SHIFT 10
#define WINDOW_OFFSET_Y_SHIFT 15
/* GetTPage's fields */
#define TPAGE_DEPTH_SHIFT 7
#define TPAGE_SEMI_SHIFT 5
#define TPAGE_X_SHIFT 6
#define TPAGE_X_MASK 0x3FF
#define TPAGE_Y_BIT 0x100
#define TPAGE_Y_SHIFT 4
#define TPAGE_FIELD_MASK 3
/* TRADEOFF: GetGraphType's answer for the GPU the PlayStation's first models carry; the game picks a video flag by it
 * (decomp/src/game/80015614.c) */
#define GRAPH_TYPE 1

static DispEnv s_Display;
static int s_DisplayEnabled;

static void send_words(const uint32_t *words, int count) {
    int index;

    for (index = 0; index < count; index++) {
        Gpu_Write(words[index]);
    }
}

int ResetGraph(int mode) {
    (void)mode;
    return 0;
}

int SetGraphDebug(int level) {
    (void)level;
    return 0;
}

int GetGraphType(void) {
    return GRAPH_TYPE;
}

DrawEnv *SetDefDrawEnv(DrawEnv *env, int x, int y, int w, int h) {
    memset(env, 0, sizeof(*env));
    env->clip.x = (int16_t)x;
    env->clip.y = (int16_t)y;
    env->clip.w = (int16_t)w;
    env->clip.h = (int16_t)h;
    env->ofs[0] = (int16_t)x;
    env->ofs[1] = (int16_t)y;
    env->tpage = DEFAULT_TPAGE;
    env->dtd = 1;
    env->dfe = 0;
    return env;
}

DispEnv *SetDefDispEnv(DispEnv *env, int x, int y, int w, int h) {
    memset(env, 0, sizeof(*env));
    env->disp.x = (int16_t)x;
    env->disp.y = (int16_t)y;
    env->disp.w = (int16_t)w;
    env->disp.h = (int16_t)h;
    return env;
}

/* A texture window's GP0 word from its rectangle: a 0 size leaves no window */
static uint32_t texture_window(const Rect *window) {
    uint32_t mask_x = window->w == 0 ? 0 : ((~(window->w - 1)) >> WINDOW_UNIT_SHIFT) & WINDOW_FIELD_MASK;
    uint32_t mask_y = window->h == 0 ? 0 : ((~(window->h - 1)) >> WINDOW_UNIT_SHIFT) & WINDOW_FIELD_MASK;
    uint32_t offset_x = ((uint32_t)window->x >> WINDOW_UNIT_SHIFT) & WINDOW_FIELD_MASK;
    uint32_t offset_y = ((uint32_t)window->y >> WINDOW_UNIT_SHIFT) & WINDOW_FIELD_MASK;

    return TEXTURE_WINDOW_COMMAND | mask_x | (mask_y << WINDOW_MASK_Y_SHIFT) | (offset_x << WINDOW_OFFSET_X_SHIFT) |
           (offset_y << WINDOW_OFFSET_Y_SHIFT);
}

DrawEnv *PutDrawEnv(DrawEnv *env) {
    uint32_t words[3];

    Gpu_SetDrawArea(env->clip.x, env->clip.y, env->clip.x + env->clip.w - 1, env->clip.y + env->clip.h - 1);
    Gpu_SetDrawOffset(env->ofs[0], env->ofs[1]);
    Gpu_Write(DRAW_MODE_COMMAND | (env->tpage & DRAW_MODE_PAGE_MASK) | (env->dtd ? DRAW_MODE_DITHER : 0) |
              (env->dfe ? DRAW_MODE_DISPLAY : 0));
    Gpu_Write(texture_window(&env->tw));
    if (env->isbg) {
        words[0] = FILL_COMMAND | env->r0 | ((uint32_t)env->g0 << COLOUR_GREEN_SHIFT) |
                   ((uint32_t)env->b0 << COLOUR_BLUE_SHIFT);
        words[1] = ((uint32_t)env->clip.x & FILL_X_MASK) | ((uint32_t)env->clip.y << HALF_SHIFT);
        words[2] = (((uint32_t)env->clip.w + FILL_WIDTH_ROUND) & HALF_MASK) | ((uint32_t)env->clip.h << HALF_SHIFT);
        send_words(words, 3);
    } else {
        /* No background fill */
    }
    return env;
}

DispEnv *PutDispEnv(DispEnv *env) {
    s_Display = *env;
    return env;
}

int SetDispMask(int mask) {
    s_DisplayEnabled = mask != 0;
    return 0;
}

/* Where libgpu last put the display, and whether it shows */
void Host_DisplayArea(int *x, int *y, int *width, int *height, int *enabled) {
    *x = s_Display.disp.x;
    *y = s_Display.disp.y;
    *width = s_Display.disp.w;
    *height = s_Display.disp.h;
    *enabled = s_DisplayEnabled;
}

static uint32_t *linked(uint32_t link) {
    return (uint32_t *)(uintptr_t)(KSEG0 | link);
}

int DrawPrim(uint32_t *primitive) {
    send_words(primitive + 1, (int)(primitive[0] >> TAG_LENGTH_SHIFT));
    return 0;
}

/* The ordering table from its first entry: each entry's words, then the one it links to, to the end mark */
int DrawOTag(uint32_t *table) {
    uint32_t *entry = table;

    for (;;) {
        uint32_t tag = entry[0];

        send_words(entry + 1, (int)(tag >> TAG_LENGTH_SHIFT));
        if ((tag & TAG_LINK_MASK) == TAG_END) {
            return 0;
        } else {
            entry = linked(tag & TAG_LINK_MASK);
        }
    }
}

int DrawSync(int mode) {
    (void)mode;
    return 0;
}

void AddPrim(uint32_t *table, uint32_t *primitive) {
    primitive[0] = (primitive[0] & ~TAG_LINK_MASK) | (table[0] & TAG_LINK_MASK);
    table[0] = (table[0] & ~TAG_LINK_MASK) | ((uint32_t)(uintptr_t)primitive & TAG_LINK_MASK);
}

void TermPrim(uint32_t *primitive) {
    primitive[0] |= TAG_END;
}

void SetPolyF4(uint32_t *primitive) {
    primitive[0] = (primitive[0] & TAG_LINK_MASK) | ((uint32_t)POLY_F4_LENGTH << TAG_LENGTH_SHIFT);
    ((uint8_t *)primitive)[sizeof(uint32_t) + CODE_BYTE] = POLY_F4_CODE;
}

void SetSemiTrans(uint32_t *primitive, int semi) {
    uint8_t *code = &((uint8_t *)primitive)[sizeof(uint32_t) + CODE_BYTE];

    if (semi) {
        *code |= SEMI_BIT;
    } else {
        *code &= (uint8_t)~SEMI_BIT;
    }
}

int GetTPage(int depth, int semi, int x, int y) {
    return ((depth & TPAGE_FIELD_MASK) << TPAGE_DEPTH_SHIFT) | ((semi & TPAGE_FIELD_MASK) << TPAGE_SEMI_SHIFT) |
           ((y & TPAGE_Y_BIT) >> TPAGE_Y_SHIFT) | ((x & TPAGE_X_MASK) >> TPAGE_X_SHIFT);
}

void SetDrawMode(uint32_t *primitive, int dfe, int dtd, int tpage, Rect *window) {
    primitive[0] = (primitive[0] & TAG_LINK_MASK) | ((uint32_t)DRAW_MODE_LENGTH << TAG_LENGTH_SHIFT);
    primitive[1] = DRAW_MODE_COMMAND | ((uint32_t)tpage & DRAW_MODE_PAGE_MASK) | (dtd ? DRAW_MODE_DITHER : 0) |
                   (dfe ? DRAW_MODE_DISPLAY : 0);
    if (window != NULL) {
        primitive[2] = texture_window(window);
    } else {
        primitive[2] = 0;
    }
}

int LoadImage(Rect *rect, uint32_t *pixels) {
    Gpu_LoadImage(rect->x, rect->y, rect->w, rect->h, (const uint16_t *)pixels);
    return 0;
}

int StoreImage(Rect *rect, uint32_t *pixels) {
    Gpu_StoreImage(rect->x, rect->y, rect->w, rect->h, (uint16_t *)pixels);
    return 0;
}

int MoveImage(Rect *rect, int x, int y) {
    Gpu_MoveImage(rect->x, rect->y, rect->w, rect->h, x, y);
    return 0;
}
