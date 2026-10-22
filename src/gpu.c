#include <string.h>

#include "gpu.h"
#include "host_log.h"

/* The GPU's GP0 commands, from psx-spx's "GPU Render Polygon Commands" to "GPU Display Control Commands": polygons
 * (flat, Gouraud, textured), lines, rectangles, fills, VRAM copies and the drawing state. A pixel is 15 bits of
 * colour and a mask bit; a texture is 4- or 8-bit indices into a CLUT, or 15-bit colours.
 * TRADEOFF: triangles are drawn by edge functions with the top-left rule and attributes interpolated per pixel; the
 * GPU's own fixed-point stepping differs by a pixel or a shade at places, for a pixel-exact picture if one is needed */

uint16_t g_Vram[VRAM_HEIGHT][VRAM_WIDTH];

#define VRAM_X_MASK (VRAM_WIDTH - 1)
#define VRAM_Y_MASK (VRAM_HEIGHT - 1)
#define COMMAND_SHIFT 24
#define COLOUR_MASK 0xFFFFFF
#define MASK_BIT 0x8000
#define CHANNEL_MAX 255
#define CHANNEL_SHIFT 3
#define FIVE_BITS 31
#define RED_SHIFT 0
#define GREEN_SHIFT 5
#define BLUE_SHIFT 10
#define BYTE_MASK 0xFF
#define COLOUR_GREEN_SHIFT 8
#define COLOUR_BLUE_SHIFT 16
/* A vertex's word: x in bits 0-10, y in bits 16-26, signed */
#define COORDINATE_BITS 11
#define HALF_SHIFT 16
#define HALF_MASK 0xFFFF
/* The largest primitive the GPU draws: wider or taller ones it skips */
#define MAX_WIDTH 1023
#define MAX_HEIGHT 511
/* Texture modulation: a colour of 128 leaves the texel as it is */
#define MODULATION_SHIFT 7
#define WORDS_MAX 32

/* The command's bits */
#define POLYGON_GOURAUD 0x10
#define POLYGON_QUAD 0x08
#define PRIMITIVE_TEXTURED 0x04
#define PRIMITIVE_SEMI 0x02
#define PRIMITIVE_RAW 0x01
#define LINE_POLY 0x08
#define RECT_SIZE_SHIFT 3
#define RECT_SIZE_MASK 3
#define POLYLINE_END_MASK 0xF000F000
#define POLYLINE_END 0x50005000

enum {
    KIND_POLYGON = 1,
    KIND_LINE = 2,
    KIND_RECT = 3
};

#define CMD_FILL 0x02
#define CMD_POLYGON 0x20
#define CMD_LINE 0x40
#define CMD_RECT 0x60
#define CMD_ENVIRONMENT 0xE0
#define CMD_COPY 0x80
#define CMD_LOAD 0xA0
#define CMD_STORE 0xC0
#define CMD_DRAW_MODE 0xE1
#define CMD_TEXTURE_WINDOW 0xE2
#define CMD_AREA_TOP_LEFT 0xE3
#define CMD_AREA_BOTTOM_RIGHT 0xE4
#define CMD_OFFSET 0xE5
#define CMD_MASK 0xE6

/* Texture page bits (E1, and a textured polygon's second texture word) */
#define TPAGE_X_MASK 0xF
#define TPAGE_X_UNIT 64
#define TPAGE_Y_BIT 0x10
#define TPAGE_Y_UNIT 256
#define TPAGE_SEMI_SHIFT 5
#define TPAGE_DEPTH_SHIFT 7
#define TPAGE_DITHER 0x200
#define TPAGE_FIELD_MASK 0x3
#define TPAGE_POLYGON_MASK 0x9FF
#define TPAGE_FLIP_X 0x1000
#define TPAGE_FLIP_Y 0x2000
#define CLUT_X_UNIT 16
#define CLUT_X_MASK 0x3F
#define CLUT_Y_SHIFT 6
#define CLUT_Y_MASK 0x1FF
/* A 4-bit texel: four to a VRAM halfword; an 8-bit one: two */
#define TEXELS_4_PER_HALF 4
#define TEXEL_4_BITS 4
#define TEXEL_4_MASK 0xF
#define TEXELS_8_PER_HALF 2
#define TEXEL_8_BITS 8
/* The texture window's fields (E2), 5 bits each in units of 8 texels */
#define WINDOW_UNIT 8
#define WINDOW_FIELD_MASK 0x1F
#define WINDOW_MASK_Y_SHIFT 5
#define WINDOW_OFFSET_X_SHIFT 10
#define WINDOW_OFFSET_Y_SHIFT 15
/* The drawing area's corners (E3, E4): x in 10 bits, then y in 9 */
#define AREA_X_MASK 0x3FF
#define AREA_Y_SHIFT 10
#define AREA_Y_MASK 0x1FF
/* The offset (E5): x and y in 11 bits each */
#define OFFSET_FIELD_MASK 0x7FF
/* The mask bits (E6) */
#define MASK_SET 1
#define MASK_CHECK 2
/* A fill's corner: x in 16-pixel steps; its width rounded up to 16 */
#define FILL_X_MASK 0x3F0
#define FILL_WIDTH_MASK 0x3FF
#define FILL_WIDTH_ROUND 0xF
/* The dither matrix's side: an offset each pixel by its x and y modulo 4 */
#define DITHER_SIDE 4
#define DITHER_MASK (DITHER_SIDE - 1)
/* Semi-transparency's 0 and 3 modes: half the sum, a quarter of the front */
#define HALF_DIVISOR 2
#define QUARTER_DIVISOR 4
#define BITS_PER_BYTE 8
/* The words of the commands of a fixed length: a rectangle's colour and corner before its texture and size */
#define RECT_BASE_WORDS 2
#define COPY_WORDS 4
#define TRANSFER_WORDS 3
#define FILL_WORDS 3
#define ENVIRONMENT_WORDS 1

enum {
    DEPTH_4 = 0,
    DEPTH_8 = 1,
    DEPTH_15 = 2
};

typedef struct {
    int x, y;
    int r, g, b;
    int u, v;
} Vertex;

typedef struct {
    int texture_x, texture_y;
    int semi_mode;
    int depth;
    int dither;
    int flip_x, flip_y;
    int window_mask_x, window_mask_y, window_offset_x, window_offset_y;
    int area_left, area_top, area_right, area_bottom;
    int offset_x, offset_y;
    int set_mask, check_mask;
} State;

/* A primitive being drawn: its texture, if any, and how it blends */
typedef struct {
    int textured;
    int raw;
    int semi;
    int dithered;
    int clut_x, clut_y;
} Paint;

static State s_State;
static uint32_t s_Words[WORDS_MAX];
static int s_Count;
/* A CPU-to-VRAM transfer in progress (GP0 0xA0): its rectangle and the pixels left */
static int s_LoadX, s_LoadY, s_LoadWidth, s_LoadHeight, s_LoadPixel, s_LoadPixels;

static const int s_Dither[DITHER_SIDE][DITHER_SIDE] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static int sign_extend(uint32_t value, int bits) {
    int shift = (int)(sizeof(int) * BITS_PER_BYTE) - bits;

    return (int)(value << shift) >> shift;
}

static int clamp_channel(int value) {
    if (value < 0) {
        return 0;
    } else if (value > CHANNEL_MAX) {
        return CHANNEL_MAX;
    } else {
        return value;
    }
}

static void set_vertex_position(Vertex *vertex, uint32_t word) {
    vertex->x = sign_extend(word & HALF_MASK, COORDINATE_BITS) + s_State.offset_x;
    vertex->y = sign_extend(word >> HALF_SHIFT, COORDINATE_BITS) + s_State.offset_y;
}

static void set_vertex_colour(Vertex *vertex, uint32_t word) {
    vertex->r = word & BYTE_MASK;
    vertex->g = (word >> COLOUR_GREEN_SHIFT) & BYTE_MASK;
    vertex->b = (word >> COLOUR_BLUE_SHIFT) & BYTE_MASK;
}

static void set_texture_page(uint32_t page) {
    s_State.texture_x = (page & TPAGE_X_MASK) * TPAGE_X_UNIT;
    s_State.texture_y = (page & TPAGE_Y_BIT) ? TPAGE_Y_UNIT : 0;
    s_State.semi_mode = (page >> TPAGE_SEMI_SHIFT) & TPAGE_FIELD_MASK;
    s_State.depth = (page >> TPAGE_DEPTH_SHIFT) & TPAGE_FIELD_MASK;
    s_State.dither = (page & TPAGE_DITHER) != 0;
}

static void set_clut(Paint *paint, uint32_t word) {
    uint32_t clut = word >> HALF_SHIFT;

    paint->clut_x = (int)(clut & CLUT_X_MASK) * CLUT_X_UNIT;
    paint->clut_y = (int)((clut >> CLUT_Y_SHIFT) & CLUT_Y_MASK);
}

/* The texel at u, v of the current page (0 where transparent) */
static uint16_t texel(const Paint *paint, int u, int v) {
    int x;
    int y;
    uint16_t word;
    int index;

    u &= BYTE_MASK;
    v &= BYTE_MASK;
    u = (u & ~(s_State.window_mask_x * WINDOW_UNIT)) |
        ((s_State.window_offset_x & s_State.window_mask_x) * WINDOW_UNIT);
    v = (v & ~(s_State.window_mask_y * WINDOW_UNIT)) |
        ((s_State.window_offset_y & s_State.window_mask_y) * WINDOW_UNIT);
    y = (s_State.texture_y + v) & VRAM_Y_MASK;
    switch (s_State.depth) {
    case DEPTH_4:
        x = (s_State.texture_x + u / TEXELS_4_PER_HALF) & VRAM_X_MASK;
        word = g_Vram[y][x];
        index = (word >> ((u % TEXELS_4_PER_HALF) * TEXEL_4_BITS)) & TEXEL_4_MASK;
        return g_Vram[paint->clut_y][(paint->clut_x + index) & VRAM_X_MASK];
    case DEPTH_8:
        x = (s_State.texture_x + u / TEXELS_8_PER_HALF) & VRAM_X_MASK;
        word = g_Vram[y][x];
        index = (word >> ((u % TEXELS_8_PER_HALF) * TEXEL_8_BITS)) & BYTE_MASK;
        return g_Vram[paint->clut_y][(paint->clut_x + index) & VRAM_X_MASK];
    default:
        x = (s_State.texture_x + u) & VRAM_X_MASK;
        return g_Vram[y][x];
    }
}

static int blend_channel(int back, int front) {
    switch (s_State.semi_mode) {
    case 0:
        return (back + front) / HALF_DIVISOR;
    case 1:
        return back + front > FIVE_BITS ? FIVE_BITS : back + front;
    case 2:
        return back - front < 0 ? 0 : back - front;
    default:
        return back + front / QUARTER_DIVISOR > FIVE_BITS ? FIVE_BITS : back + front / QUARTER_DIVISOR;
    }
}

/* One pixel at x, y of colour r, g, b (8 bits each) and texel u, v, as the paint has it */
static void plot(const Paint *paint, int x, int y, int r, int g, int b, int u, int v) {
    uint16_t *target;
    uint16_t source = 0;
    int mask = 0;
    int semi = paint->semi;
    int red, green, blue;

    if (x < s_State.area_left || x > s_State.area_right || y < s_State.area_top || y > s_State.area_bottom) {
        return;
    } else {
        target = &g_Vram[y & VRAM_Y_MASK][x & VRAM_X_MASK];
    }
    if (s_State.check_mask && (*target & MASK_BIT)) {
        return;
    } else if (paint->textured) {
        source = texel(paint, u, v);
        if (source == 0) {
            return;
        } else {
            mask = source & MASK_BIT;
            semi = semi && mask;
        }
    } else {
        /* Untextured: the vertex colour alone */
    }
    if (!paint->textured) {
        red = r;
        green = g;
        blue = b;
    } else if (paint->raw) {
        red = ((source >> RED_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT;
        green = ((source >> GREEN_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT;
        blue = ((source >> BLUE_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT;
    } else {
        red = ((((source >> RED_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT) * r) >> MODULATION_SHIFT;
        green = ((((source >> GREEN_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT) * g) >> MODULATION_SHIFT;
        blue = ((((source >> BLUE_SHIFT) & FIVE_BITS) << CHANNEL_SHIFT) * b) >> MODULATION_SHIFT;
    }
    if (paint->dithered && s_State.dither) {
        int offset = s_Dither[y & DITHER_MASK][x & DITHER_MASK];

        red += offset;
        green += offset;
        blue += offset;
    } else {
        /* No dithering */
    }
    red = clamp_channel(red) >> CHANNEL_SHIFT;
    green = clamp_channel(green) >> CHANNEL_SHIFT;
    blue = clamp_channel(blue) >> CHANNEL_SHIFT;
    if (semi) {
        red = blend_channel((*target >> RED_SHIFT) & FIVE_BITS, red);
        green = blend_channel((*target >> GREEN_SHIFT) & FIVE_BITS, green);
        blue = blend_channel((*target >> BLUE_SHIFT) & FIVE_BITS, blue);
    } else {
        /* Opaque */
    }
    *target = (uint16_t)((red << RED_SHIFT) | (green << GREEN_SHIFT) | (blue << BLUE_SHIFT) | mask |
                         (s_State.set_mask ? MASK_BIT : 0));
}

static long long edge(const Vertex *a, const Vertex *b, int x, int y) {
    return (long long)(b->x - a->x) * (y - a->y) - (long long)(b->y - a->y) * (x - a->x);
}

/* An edge a-b of a clockwise (on screen) triangle is a top or left one: its pixels are drawn */
static int top_left(const Vertex *a, const Vertex *b) {
    return (a->y == b->y && b->x > a->x) || b->y < a->y;
}

static int min3(int a, int b, int c) {
    int low = a < b ? a : b;

    return low < c ? low : c;
}

static int max3(int a, int b, int c) {
    int high = a > b ? a : b;

    return high > c ? high : c;
}

/* An attribute at a pixel, from its vertices' values weighted by the pixel's edge functions */
static int interpolate(long long w0, long long w1, long long w2, int a, int b, int c, long long area) {
    return (int)((w0 * a + w1 * b + w2 * c) / area);
}

static void triangle(const Paint *paint, const Vertex *v0, const Vertex *v1, const Vertex *v2, int gouraud) {
    const Vertex *a = v0;
    const Vertex *b = v1;
    const Vertex *c = v2;
    long long area = edge(v0, v1, v2->x, v2->y);
    int left, right, top, bottom;
    int x, y;
    int bias0, bias1, bias2;

    if (area == 0) {
        return;
    } else if (area < 0) {
        b = v2;
        c = v1;
        area = -area;
    } else {
        /* Already clockwise */
    }
    left = min3(a->x, b->x, c->x);
    right = max3(a->x, b->x, c->x);
    top = min3(a->y, b->y, c->y);
    bottom = max3(a->y, b->y, c->y);
    if (right - left > MAX_WIDTH || bottom - top > MAX_HEIGHT) {
        return;
    } else {
        left = left > s_State.area_left ? left : s_State.area_left;
        right = right < s_State.area_right ? right : s_State.area_right;
        top = top > s_State.area_top ? top : s_State.area_top;
        bottom = bottom < s_State.area_bottom ? bottom : s_State.area_bottom;
    }
    bias0 = top_left(b, c) ? 0 : -1;
    bias1 = top_left(c, a) ? 0 : -1;
    bias2 = top_left(a, b) ? 0 : -1;
    for (y = top; y <= bottom; y++) {
        for (x = left; x <= right; x++) {
            long long w0 = edge(b, c, x, y);
            long long w1 = edge(c, a, x, y);
            long long w2 = edge(a, b, x, y);

            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) {
                continue;
            } else if (gouraud) {
                plot(paint, x, y, interpolate(w0, w1, w2, a->r, b->r, c->r, area),
                     interpolate(w0, w1, w2, a->g, b->g, c->g, area), interpolate(w0, w1, w2, a->b, b->b, c->b, area),
                     interpolate(w0, w1, w2, a->u, b->u, c->u, area), interpolate(w0, w1, w2, a->v, b->v, c->v, area));
            } else {
                plot(paint, x, y, a->r, a->g, a->b, interpolate(w0, w1, w2, a->u, b->u, c->u, area),
                     interpolate(w0, w1, w2, a->v, b->v, c->v, area));
            }
        }
    }
}

static int polygon_words(uint32_t command) {
    int vertices = (command & POLYGON_QUAD) ? 4 : 3;
    int per_vertex = (command & PRIMITIVE_TEXTURED) ? 2 : 1;

    return 1 + vertices * per_vertex + ((command & POLYGON_GOURAUD) ? vertices - 1 : 0);
}

static void polygon(const uint32_t *words) {
    uint32_t command = words[0] >> COMMAND_SHIFT;
    int vertices = (command & POLYGON_QUAD) ? 4 : 3;
    int gouraud = (command & POLYGON_GOURAUD) != 0;
    Vertex vertex[4];
    Paint paint;
    int index = 0;
    int i;

    memset(&paint, 0, sizeof(paint));
    paint.textured = (command & PRIMITIVE_TEXTURED) != 0;
    paint.raw = (command & PRIMITIVE_RAW) != 0;
    paint.semi = (command & PRIMITIVE_SEMI) != 0;
    paint.dithered = gouraud || (paint.textured && !paint.raw);
    for (i = 0; i < vertices; i++) {
        if (i == 0) {
            set_vertex_colour(&vertex[i], words[0]);
            index = 1;
        } else if (gouraud) {
            set_vertex_colour(&vertex[i], words[index++]);
        } else {
            set_vertex_colour(&vertex[i], words[0]);
        }
        set_vertex_position(&vertex[i], words[index++]);
        if (paint.textured) {
            vertex[i].u = words[index] & BYTE_MASK;
            vertex[i].v = (words[index] >> COLOUR_GREEN_SHIFT) & BYTE_MASK;
            if (i == 0) {
                set_clut(&paint, words[index]);
            } else if (i == 1) {
                set_texture_page((words[index] >> HALF_SHIFT) & TPAGE_POLYGON_MASK);
            } else {
                /* The other vertices' upper halves are unused */
            }
            index++;
        } else {
            vertex[i].u = 0;
            vertex[i].v = 0;
        }
    }
    triangle(&paint, &vertex[0], &vertex[1], &vertex[2], gouraud);
    if (vertices == 4) {
        triangle(&paint, &vertex[1], &vertex[2], &vertex[3], gouraud);
    } else {
        /* A triangle */
    }
}

static int magnitude(int value) {
    if (value < 0) {
        return -value;
    } else {
        return value;
    }
}

/* A line, both ends drawn, a pixel each step along its longer axis.
 * TRADEOFF: positions truncated toward the start; the GPU's own stepping rounds otherwise at places */
static void line(const Vertex *from, const Vertex *to, int semi, int gouraud) {
    Paint paint;
    int dx = to->x - from->x;
    int dy = to->y - from->y;
    int steps = magnitude(dx) > magnitude(dy) ? magnitude(dx) : magnitude(dy);
    int step;

    memset(&paint, 0, sizeof(paint));
    paint.semi = semi;
    paint.dithered = gouraud;
    if (magnitude(dx) > MAX_WIDTH || magnitude(dy) > MAX_HEIGHT) {
        return;
    } else if (steps == 0) {
        plot(&paint, from->x, from->y, from->r, from->g, from->b, 0, 0);
    } else {
        for (step = 0; step <= steps; step++) {
            plot(&paint, from->x + dx * step / steps, from->y + dy * step / steps,
                 from->r + (to->r - from->r) * step / steps, from->g + (to->g - from->g) * step / steps,
                 from->b + (to->b - from->b) * step / steps, 0, 0);
        }
    }
}

static int line_words(uint32_t command) {
    return (command & POLYGON_GOURAUD) ? 4 : 3;
}

static void single_line(const uint32_t *words) {
    uint32_t command = words[0] >> COMMAND_SHIFT;
    int gouraud = (command & POLYGON_GOURAUD) != 0;
    Vertex from;
    Vertex to;

    set_vertex_colour(&from, words[0]);
    set_vertex_position(&from, words[1]);
    set_vertex_colour(&to, gouraud ? words[2] : words[0]);
    set_vertex_position(&to, words[gouraud ? 3 : 2]);
    line(&from, &to, (command & PRIMITIVE_SEMI) != 0, gouraud);
}

static int rect_words(uint32_t command) {
    int size = (command >> RECT_SIZE_SHIFT) & RECT_SIZE_MASK;

    return RECT_BASE_WORDS + ((command & PRIMITIVE_TEXTURED) ? 1 : 0) + (size == 0 ? 1 : 0);
}

static void rect(const uint32_t *words) {
    static const int sizes[] = {0, 1, 8, 16};
    uint32_t command = words[0] >> COMMAND_SHIFT;
    int size = (command >> RECT_SIZE_SHIFT) & RECT_SIZE_MASK;
    Paint paint;
    Vertex corner;
    int index = 2;
    int width;
    int height;
    int u0 = 0;
    int v0 = 0;
    int x, y;

    memset(&paint, 0, sizeof(paint));
    paint.textured = (command & PRIMITIVE_TEXTURED) != 0;
    paint.raw = (command & PRIMITIVE_RAW) != 0;
    paint.semi = (command & PRIMITIVE_SEMI) != 0;
    set_vertex_colour(&corner, words[0]);
    set_vertex_position(&corner, words[1]);
    if (paint.textured) {
        u0 = words[index] & BYTE_MASK;
        v0 = (words[index] >> COLOUR_GREEN_SHIFT) & BYTE_MASK;
        set_clut(&paint, words[index]);
        index++;
    } else {
        /* Untextured */
    }
    if (size == 0) {
        width = words[index] & HALF_MASK;
        height = words[index] >> HALF_SHIFT;
    } else {
        width = sizes[size];
        height = sizes[size];
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            int u = s_State.flip_x ? u0 - x : u0 + x;
            int v = s_State.flip_y ? v0 - y : v0 + y;

            plot(&paint, corner.x + x, corner.y + y, corner.r, corner.g, corner.b, u, v);
        }
    }
}

static void fill(const uint32_t *words) {
    Vertex colour;
    int left = (int)(words[1] & FILL_X_MASK);
    int top = (int)((words[1] >> HALF_SHIFT) & VRAM_Y_MASK);
    int width = (int)(((words[2] & FILL_WIDTH_MASK) + FILL_WIDTH_ROUND) & ~FILL_WIDTH_ROUND);
    int height = (int)((words[2] >> HALF_SHIFT) & VRAM_Y_MASK);
    uint16_t pixel;
    int x, y;

    set_vertex_colour(&colour, words[0]);
    pixel = (uint16_t)(((colour.r >> CHANNEL_SHIFT) << RED_SHIFT) | ((colour.g >> CHANNEL_SHIFT) << GREEN_SHIFT) |
                       ((colour.b >> CHANNEL_SHIFT) << BLUE_SHIFT));
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            g_Vram[(top + y) & VRAM_Y_MASK][(left + x) & VRAM_X_MASK] = pixel;
        }
    }
}

void Gpu_MoveImage(int x, int y, int width, int height, int to_x, int to_y) {
    int row, column;

    for (row = 0; row < height; row++) {
        for (column = 0; column < width; column++) {
            g_Vram[(to_y + row) & VRAM_Y_MASK][(to_x + column) & VRAM_X_MASK] =
                g_Vram[(y + row) & VRAM_Y_MASK][(x + column) & VRAM_X_MASK];
        }
    }
}

void Gpu_LoadImage(int x, int y, int width, int height, const uint16_t *pixels) {
    int row, column;

    for (row = 0; row < height; row++) {
        for (column = 0; column < width; column++) {
            g_Vram[(y + row) & VRAM_Y_MASK][(x + column) & VRAM_X_MASK] = pixels[row * width + column];
        }
    }
}

void Gpu_StoreImage(int x, int y, int width, int height, uint16_t *pixels) {
    int row, column;

    for (row = 0; row < height; row++) {
        for (column = 0; column < width; column++) {
            pixels[row * width + column] = g_Vram[(y + row) & VRAM_Y_MASK][(x + column) & VRAM_X_MASK];
        }
    }
}

void Gpu_SetDrawArea(int left, int top, int right, int bottom) {
    s_State.area_left = left;
    s_State.area_top = top;
    s_State.area_right = right;
    s_State.area_bottom = bottom;
}

void Gpu_SetDrawOffset(int x, int y) {
    s_State.offset_x = x;
    s_State.offset_y = y;
}

/* The words a command takes, or 0 for a polyline (ended by its terminator) */
static int command_words(uint32_t command) {
    if (command >= CMD_POLYGON && command < CMD_LINE) {
        return polygon_words(command);
    } else if (command >= CMD_LINE && command < CMD_RECT) {
        return (command & LINE_POLY) ? 0 : line_words(command);
    } else if (command >= CMD_RECT && command < CMD_COPY) {
        return rect_words(command);
    } else if (command >= CMD_COPY && command < CMD_LOAD) {
        return COPY_WORDS;
    } else if (command >= CMD_LOAD && command < CMD_ENVIRONMENT) {
        return TRANSFER_WORDS;
    } else if (command == CMD_FILL) {
        return FILL_WORDS;
    } else {
        return ENVIRONMENT_WORDS;
    }
}

static void environment(uint32_t word) {
    uint32_t command = word >> COMMAND_SHIFT;

    switch (command) {
    case CMD_DRAW_MODE:
        set_texture_page(word & TPAGE_POLYGON_MASK);
        s_State.flip_x = (word & TPAGE_FLIP_X) != 0;
        s_State.flip_y = (word & TPAGE_FLIP_Y) != 0;
        break;
    case CMD_TEXTURE_WINDOW:
        s_State.window_mask_x = word & WINDOW_FIELD_MASK;
        s_State.window_mask_y = (word >> WINDOW_MASK_Y_SHIFT) & WINDOW_FIELD_MASK;
        s_State.window_offset_x = (word >> WINDOW_OFFSET_X_SHIFT) & WINDOW_FIELD_MASK;
        s_State.window_offset_y = (word >> WINDOW_OFFSET_Y_SHIFT) & WINDOW_FIELD_MASK;
        break;
    case CMD_AREA_TOP_LEFT:
        s_State.area_left = word & AREA_X_MASK;
        s_State.area_top = (word >> AREA_Y_SHIFT) & AREA_Y_MASK;
        break;
    case CMD_AREA_BOTTOM_RIGHT:
        s_State.area_right = word & AREA_X_MASK;
        s_State.area_bottom = (word >> AREA_Y_SHIFT) & AREA_Y_MASK;
        break;
    case CMD_OFFSET:
        s_State.offset_x = sign_extend(word & OFFSET_FIELD_MASK, COORDINATE_BITS);
        s_State.offset_y = sign_extend((word >> COORDINATE_BITS) & OFFSET_FIELD_MASK, COORDINATE_BITS);
        break;
    case CMD_MASK:
        s_State.set_mask = (word & MASK_SET) != 0;
        s_State.check_mask = (word & MASK_CHECK) != 0;
        break;
    default:
        /* nop, cache clear, interrupt request: nothing drawn */
        break;
    }
}

static void execute(const uint32_t *words) {
    uint32_t command = words[0] >> COMMAND_SHIFT;

    if (command >= CMD_POLYGON && command < CMD_LINE) {
        polygon(words);
    } else if (command >= CMD_LINE && command < CMD_RECT) {
        single_line(words);
    } else if (command >= CMD_RECT && command < CMD_COPY) {
        rect(words);
    } else if (command >= CMD_COPY && command < CMD_LOAD) {
        Gpu_MoveImage(words[1] & VRAM_X_MASK, (words[1] >> HALF_SHIFT) & VRAM_Y_MASK,
                      ((words[3] - 1) & VRAM_X_MASK) + 1, (((words[3] >> HALF_SHIFT) - 1) & VRAM_Y_MASK) + 1,
                      words[2] & VRAM_X_MASK, (words[2] >> HALF_SHIFT) & VRAM_Y_MASK);
    } else if (command >= CMD_LOAD && command < CMD_STORE) {
        s_LoadX = words[1] & VRAM_X_MASK;
        s_LoadY = (words[1] >> HALF_SHIFT) & VRAM_Y_MASK;
        s_LoadWidth = ((words[2] - 1) & VRAM_X_MASK) + 1;
        s_LoadHeight = (((words[2] >> HALF_SHIFT) - 1) & VRAM_Y_MASK) + 1;
        s_LoadPixel = 0;
        s_LoadPixels = s_LoadWidth * s_LoadHeight;
    } else if (command >= CMD_STORE && command < CMD_ENVIRONMENT) {
        /* VRAM to the CPU: StoreImage reads VRAM itself */
    } else if (command == CMD_FILL) {
        fill(words);
    } else {
        environment(words[0]);
    }
}

static void load_pixel(uint16_t pixel) {
    int row = s_LoadPixel / s_LoadWidth;
    int column = s_LoadPixel % s_LoadWidth;

    g_Vram[(s_LoadY + row) & VRAM_Y_MASK][(s_LoadX + column) & VRAM_X_MASK] = pixel;
    s_LoadPixel++;
}

/* A polyline in progress: its last vertex, and whether a Gouraud one's next word is a colour */
static Vertex s_PolylineLast;
static Vertex s_PolylineNext;
static int s_PolylineExpectsColour;

/* A polyline's word after its command: its first vertex, then each next one's colour (Gouraud) and position, a
 * segment drawn at each position; a word of POLYLINE_END's pattern ends it */
static void polyline_word(uint32_t word) {
    uint32_t command = s_Words[0] >> COMMAND_SHIFT;
    int gouraud = (command & POLYGON_GOURAUD) != 0;

    if (s_Count == 1) {
        set_vertex_colour(&s_PolylineLast, s_Words[0]);
        set_vertex_position(&s_PolylineLast, word);
        s_PolylineExpectsColour = gouraud;
        s_Count = 2;
    } else if ((word & POLYLINE_END_MASK) == POLYLINE_END) {
        s_Count = 0;
    } else if (s_PolylineExpectsColour) {
        set_vertex_colour(&s_PolylineNext, word);
        s_PolylineExpectsColour = 0;
    } else {
        if (!gouraud) {
            set_vertex_colour(&s_PolylineNext, s_Words[0]);
        } else {
            /* Its colour came in the word before */
        }
        set_vertex_position(&s_PolylineNext, word);
        line(&s_PolylineLast, &s_PolylineNext, (command & PRIMITIVE_SEMI) != 0, gouraud);
        s_PolylineLast = s_PolylineNext;
        s_PolylineExpectsColour = gouraud;
    }
}

void Gpu_Write(uint32_t word) {
    if (s_LoadPixel < s_LoadPixels) {
        load_pixel((uint16_t)(word & HALF_MASK));
        if (s_LoadPixel < s_LoadPixels) {
            load_pixel((uint16_t)(word >> HALF_SHIFT));
        } else {
            /* The transfer's last pixel was the low half */
        }
        return;
    } else if (s_Count > 0 && command_words(s_Words[0] >> COMMAND_SHIFT) == 0) {
        polyline_word(word);
        return;
    } else {
        s_Words[s_Count++] = word;
    }
    if (command_words(s_Words[0] >> COMMAND_SHIFT) == 0) {
        /* A polyline's first word */
    } else if (s_Count == command_words(s_Words[0] >> COMMAND_SHIFT)) {
        execute(s_Words);
        s_Count = 0;
    } else {
        /* More words to come */
    }
}
