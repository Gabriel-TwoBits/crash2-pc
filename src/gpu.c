#include "gpu.h"
#include "gpu_raster.h"
#include "host_log.h"
#include "render.h"

/* The GPU's GP0 commands (psx-spx's "GPU Render Polygon Commands" to "GPU Display Control Commands"): this file reads
 * them and keeps the drawing state (E1 to E6), and SwanStation's rasterizer draws (gpu_raster.cpp), each primitive
 * handed over as SwanStation's own front end does (its GPU_SW::DispatchRenderCommand); the GPU renderer
 * (render.c) draws each again at its resolution */

uint16_t g_Vram[VRAM_HEIGHT][VRAM_WIDTH];

#define VRAM_X_MASK (VRAM_WIDTH - 1)
#define VRAM_Y_MASK (VRAM_HEIGHT - 1)
#define COMMAND_SHIFT 24
#define COLOUR_MASK 0xFFFFFF
#define HALF_SHIFT 16
#define HALF_MASK 0xFFFF
/* A vertex's word: x in bits 0-10, y in bits 16-26, signed */
#define COORDINATE_BITS 11
#define WORDS_MAX 256
#define BITS_PER_BYTE 8

/* The command's bits */
#define POLYGON_GOURAUD 0x10
#define POLYGON_QUAD 0x08
#define PRIMITIVE_TEXTURED 0x04
#define LINE_POLY 0x08
#define RECT_SIZE_SHIFT 3
#define RECT_SIZE_MASK 3
#define POLYLINE_END_MASK 0xF000F000
#define POLYLINE_END 0x50005000

#define CMD_FILL 0x02
#define CMD_POLYGON 0x20
#define CMD_LINE 0x40
#define CMD_RECT 0x60
#define CMD_COPY 0x80
#define CMD_LOAD 0xA0
#define CMD_STORE 0xC0
#define CMD_ENVIRONMENT 0xE0
#define CMD_DRAW_MODE 0xE1
#define CMD_TEXTURE_WINDOW 0xE2
#define CMD_AREA_TOP_LEFT 0xE3
#define CMD_AREA_BOTTOM_RIGHT 0xE4
#define CMD_OFFSET 0xE5
#define CMD_MASK 0xE6

/* E1's bits the draw mode keeps (the page, blending, depth, dither, display, texture off, flips), and those a textured
 * polygon's page sets (SwanStation's GPUDrawModeReg) */
#define DRAW_MODE_MASK 0x3FFF
#define POLYGON_PAGE_MASK 0x9FF
#define TEXTURE_WINDOW_MASK 0xFFFFF
#define MASK_BITS 3
/* The drawing area's corners (E3, E4): x in 10 bits, then y in 9 */
#define AREA_X_MASK 0x3FF
#define AREA_Y_SHIFT 10
#define AREA_Y_MASK 0x1FF
/* The offset (E5): x and y in 11 bits each */
#define OFFSET_FIELD_MASK 0x7FF
/* A fill's corner: x in 16-pixel steps; its width rounded up to 16 */
#define FILL_X_MASK 0x3F0
#define FILL_WIDTH_ROUND 0xF
/* The largest rectangle the GPU draws */
#define MAX_WIDTH 1024
#define MAX_HEIGHT 512
/* A VRAM copy's or transfer's size of 0 counts as the whole span */
#define WHOLE_WIDTH 0x400
#define WHOLE_HEIGHT 0x200

/* The words of the commands of a fixed length: a rectangle's colour and corner before its texture and size */
#define RECT_BASE_WORDS 2
#define COPY_WORDS 4
#define TRANSFER_WORDS 3
#define FILL_WORDS 3
#define ENVIRONMENT_WORDS 1
#define LINE_WORDS 3
#define GOURAUD_LINE_WORDS 4
#define LINE_ENDS 2
#define QUAD_VERTICES 4
#define TRIANGLE_VERTICES 3

typedef struct {
    uint16_t draw_mode;
    uint32_t texture_window;
    uint32_t mask;
    int area_left, area_top, area_right, area_bottom;
    int offset_x, offset_y;
} State;

static State s_State;
static int s_Started;
static uint32_t s_Words[WORDS_MAX];
static int s_Count;
/* A CPU-to-VRAM transfer in progress (GP0 0xA0): its rectangle and the pixels so far */
static int s_LoadX, s_LoadY, s_LoadWidth, s_LoadHeight, s_LoadPixel, s_LoadPixels;
static uint16_t s_LoadBuffer[VRAM_WIDTH * VRAM_HEIGHT];
/* A polyline in progress: its vertices so far, and whether a Gouraud one's next word is a colour */
static RasterVertex s_Polyline[WORDS_MAX];
static int s_PolylineVertices;
static int s_PolylineExpectsColour;
static uint32_t s_PolylineColour;

static void start(void) {
    if (!s_Started) {
        Raster_Init(&g_Vram[0][0]);
        s_Started = 1;
    } else {
        /* Started */
    }
}

static int sign_extend(uint32_t value, int bits) {
    int shift = (int)(sizeof(int) * BITS_PER_BYTE) - bits;

    return (int)(value << shift) >> shift;
}

static RasterVertex vertex(uint32_t position, uint32_t colour) {
    RasterVertex result;

    result.x = s_State.offset_x + sign_extend(position & HALF_MASK, COORDINATE_BITS);
    result.y = s_State.offset_y + sign_extend(position >> HALF_SHIFT, COORDINATE_BITS);
    result.color = colour & COLOUR_MASK;
    result.texcoord = 0;
    result.position = position;
    return result;
}

static RasterState draw_state(uint32_t command) {
    RasterState state;

    state.command = command;
    state.draw_mode = s_State.draw_mode;
    state.palette = 0;
    state.texture_window = s_State.texture_window;
    state.mask = s_State.mask;
    return state;
}

static int drawing_area_valid(void) {
    return s_State.area_left <= s_State.area_right && s_State.area_top <= s_State.area_bottom;
}

static int polygon_words(uint32_t command) {
    int vertices = (command & POLYGON_QUAD) ? QUAD_VERTICES : TRIANGLE_VERTICES;
    int per_vertex = (command & PRIMITIVE_TEXTURED) ? 2 : 1;

    return 1 + vertices * per_vertex + ((command & POLYGON_GOURAUD) ? vertices - 1 : 0);
}

/* A polygon: each vertex's colour (the first's in the command, the others' before them if Gouraud), position and
 * texture coordinates; the first's carry the CLUT, the second's the page, which the draw mode takes */
static void polygon(const uint32_t *words) {
    uint32_t command = words[0] >> COMMAND_SHIFT;
    int vertices = (command & POLYGON_QUAD) ? QUAD_VERTICES : TRIANGLE_VERTICES;
    int gouraud = (command & POLYGON_GOURAUD) != 0;
    int textured = (command & PRIMITIVE_TEXTURED) != 0;
    RasterVertex corners[QUAD_VERTICES];
    RasterState state = draw_state(words[0]);
    int index = 1;
    int i;

    for (i = 0; i < vertices; i++) {
        uint32_t colour;

        if (i > 0 && gouraud) {
            colour = words[index++];
        } else {
            colour = words[0];
        }
        corners[i] = vertex(words[index++], colour);
        if (!textured) {
            continue;
        } else if (i == 0) {
            state.palette = (uint16_t)(words[index] >> HALF_SHIFT);
        } else if (i == 1) {
            s_State.draw_mode = (uint16_t)((s_State.draw_mode & ~POLYGON_PAGE_MASK) |
                                           ((words[index] >> HALF_SHIFT) & POLYGON_PAGE_MASK));
            state.draw_mode = s_State.draw_mode;
        } else {
            /* The other vertices' upper halves are unused */
        }
        corners[i].texcoord = (uint16_t)(words[index] & HALF_MASK);
        index++;
    }
    if (drawing_area_valid()) {
        Raster_Polygon(&state, vertices, corners);
        Render_Polygon(&state, vertices, corners);
    } else {
        /* Nothing is drawn outside a valid area */
    }
}

static int line_words(uint32_t command) {
    return (command & POLYGON_GOURAUD) ? GOURAUD_LINE_WORDS : LINE_WORDS;
}

static void single_line(const uint32_t *words) {
    uint32_t command = words[0] >> COMMAND_SHIFT;
    RasterVertex ends[LINE_ENDS];
    RasterState state = draw_state(words[0]);

    if (command & POLYGON_GOURAUD) {
        ends[0] = vertex(words[1], words[0]);
        ends[1] = vertex(words[3], words[2]);
    } else {
        ends[0] = vertex(words[1], words[0]);
        ends[1] = vertex(words[2], words[0]);
    }
    if (drawing_area_valid()) {
        Raster_Line(&state, LINE_ENDS, ends);
        Render_Line(&state, LINE_ENDS, ends);
    } else {
        /* Nothing is drawn outside a valid area */
    }
}

static int rect_words(uint32_t command) {
    int size = (command >> RECT_SIZE_SHIFT) & RECT_SIZE_MASK;

    return RECT_BASE_WORDS + ((command & PRIMITIVE_TEXTURED) ? 1 : 0) + (size == 0 ? 1 : 0);
}

/* A rectangle: its corner (11 bits after the offset), its texture coordinates and CLUT, its size (fixed or given) */
static void rect(const uint32_t *words) {
    static const uint16_t sizes[] = {0, 1, 8, 16};
    uint32_t command = words[0] >> COMMAND_SHIFT;
    int size = (command >> RECT_SIZE_SHIFT) & RECT_SIZE_MASK;
    RasterState state = draw_state(words[0]);
    RasterVertex corner = vertex(words[1], words[0]);
    uint16_t texcoord = 0;
    uint16_t width;
    uint16_t height;
    int index = RECT_BASE_WORDS;

    if (command & PRIMITIVE_TEXTURED) {
        state.palette = (uint16_t)(words[index] >> HALF_SHIFT);
        texcoord = (uint16_t)(words[index] & HALF_MASK);
        index++;
    } else {
        /* Untextured */
    }
    if (size == 0) {
        width = (uint16_t)(words[index] & VRAM_X_MASK);
        height = (uint16_t)((words[index] >> HALF_SHIFT) & VRAM_Y_MASK);
    } else {
        width = sizes[size];
        height = sizes[size];
    }
    if (width >= MAX_WIDTH || height >= MAX_HEIGHT || !drawing_area_valid()) {
        return;
    } else {
        Raster_Rectangle(&state, sign_extend((uint32_t)corner.x, COORDINATE_BITS),
                         sign_extend((uint32_t)corner.y, COORDINATE_BITS), width, height, texcoord);
        Render_Rectangle(&state, sign_extend((uint32_t)corner.x, COORDINATE_BITS),
                         sign_extend((uint32_t)corner.y, COORDINATE_BITS), width, height, texcoord);
    }
}

static void fill(const uint32_t *words) {
    uint32_t x = words[1] & FILL_X_MASK;
    uint32_t y = (words[1] >> HALF_SHIFT) & VRAM_Y_MASK;
    uint32_t width = ((words[2] & VRAM_X_MASK) + FILL_WIDTH_ROUND) & ~(uint32_t)FILL_WIDTH_ROUND;
    uint32_t height = (words[2] >> HALF_SHIFT) & VRAM_Y_MASK;

    if (width > 0 && height > 0) {
        Raster_Fill(x, y, width, height, words[0] & COLOUR_MASK);
        Render_Fill(x, y, width, height, words[0] & COLOUR_MASK);
    } else {
        /* An empty fill */
    }
}

static uint32_t span(uint32_t value, uint32_t mask, uint32_t whole) {
    if ((value & mask) == 0) {
        return whole;
    } else {
        return value & mask;
    }
}

void Gpu_MoveImage(int x, int y, int width, int height, int to_x, int to_y) {
    start();
    if (width > 0 && height > 0) {
        Raster_Copy((uint32_t)x & VRAM_X_MASK, (uint32_t)y & VRAM_Y_MASK, (uint32_t)to_x & VRAM_X_MASK,
                    (uint32_t)to_y & VRAM_Y_MASK, (uint32_t)width, (uint32_t)height, s_State.mask);
        Render_Copy((uint32_t)x & VRAM_X_MASK, (uint32_t)y & VRAM_Y_MASK, (uint32_t)to_x & VRAM_X_MASK,
                    (uint32_t)to_y & VRAM_Y_MASK, (uint32_t)width, (uint32_t)height);
    } else {
        /* An empty copy */
    }
}

void Gpu_LoadImage(int x, int y, int width, int height, const uint16_t *pixels) {
    start();
    if (width > 0 && height > 0) {
        Raster_Load((uint32_t)x & VRAM_X_MASK, (uint32_t)y & VRAM_Y_MASK, (uint32_t)width, (uint32_t)height, pixels,
                    s_State.mask);
        Render_Load((uint32_t)x & VRAM_X_MASK, (uint32_t)y & VRAM_Y_MASK, (uint32_t)width, (uint32_t)height);
    } else {
        /* An empty transfer */
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

static void set_drawing_area(void) {
    if (drawing_area_valid()) {
        Raster_SetDrawingArea((uint32_t)s_State.area_left, (uint32_t)s_State.area_top, (uint32_t)s_State.area_right,
                              (uint32_t)s_State.area_bottom);
        Render_SetDrawingArea((uint32_t)s_State.area_left, (uint32_t)s_State.area_top, (uint32_t)s_State.area_right,
                              (uint32_t)s_State.area_bottom);
    } else {
        /* An invalid area draws nothing (drawing_area_valid) */
    }
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
    switch (word >> COMMAND_SHIFT) {
    case CMD_DRAW_MODE:
        s_State.draw_mode = (uint16_t)(word & DRAW_MODE_MASK);
        break;
    case CMD_TEXTURE_WINDOW:
        s_State.texture_window = word & TEXTURE_WINDOW_MASK;
        break;
    case CMD_AREA_TOP_LEFT:
        s_State.area_left = (int)(word & AREA_X_MASK);
        s_State.area_top = (int)((word >> AREA_Y_SHIFT) & AREA_Y_MASK);
        set_drawing_area();
        break;
    case CMD_AREA_BOTTOM_RIGHT:
        s_State.area_right = (int)(word & AREA_X_MASK);
        s_State.area_bottom = (int)((word >> AREA_Y_SHIFT) & AREA_Y_MASK);
        set_drawing_area();
        break;
    case CMD_OFFSET:
        s_State.offset_x = sign_extend(word & OFFSET_FIELD_MASK, COORDINATE_BITS);
        s_State.offset_y = sign_extend((word >> COORDINATE_BITS) & OFFSET_FIELD_MASK, COORDINATE_BITS);
        break;
    case CMD_MASK:
        s_State.mask = word & MASK_BITS;
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
        Raster_Copy(words[1] & VRAM_X_MASK, (words[1] >> HALF_SHIFT) & VRAM_Y_MASK, words[2] & VRAM_X_MASK,
                    (words[2] >> HALF_SHIFT) & VRAM_Y_MASK, span(words[3], VRAM_X_MASK, WHOLE_WIDTH),
                    span(words[3] >> HALF_SHIFT, VRAM_Y_MASK, WHOLE_HEIGHT), s_State.mask);
        Render_Copy(words[1] & VRAM_X_MASK, (words[1] >> HALF_SHIFT) & VRAM_Y_MASK, words[2] & VRAM_X_MASK,
                    (words[2] >> HALF_SHIFT) & VRAM_Y_MASK, span(words[3], VRAM_X_MASK, WHOLE_WIDTH),
                    span(words[3] >> HALF_SHIFT, VRAM_Y_MASK, WHOLE_HEIGHT));
    } else if (command >= CMD_LOAD && command < CMD_STORE) {
        s_LoadX = (int)(words[1] & VRAM_X_MASK);
        s_LoadY = (int)((words[1] >> HALF_SHIFT) & VRAM_Y_MASK);
        s_LoadWidth = (int)span(words[2], VRAM_X_MASK, WHOLE_WIDTH);
        s_LoadHeight = (int)span(words[2] >> HALF_SHIFT, VRAM_Y_MASK, WHOLE_HEIGHT);
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

/* A transfer's pixel, the rectangle loaded once its last comes */
static void load_pixel(uint16_t pixel) {
    s_LoadBuffer[s_LoadPixel++] = pixel;
    if (s_LoadPixel == s_LoadPixels) {
        Raster_Load((uint32_t)s_LoadX, (uint32_t)s_LoadY, (uint32_t)s_LoadWidth, (uint32_t)s_LoadHeight, s_LoadBuffer,
                    s_State.mask);
        Render_Load((uint32_t)s_LoadX, (uint32_t)s_LoadY, (uint32_t)s_LoadWidth, (uint32_t)s_LoadHeight);
    } else {
        /* More pixels to come */
    }
}

/* A polyline's word after its command: its first vertex, then each next one's colour (Gouraud) and position; a word
 * of POLYLINE_END's pattern ends it, and the whole line is drawn */
static void polyline_word(uint32_t word) {
    uint32_t command = s_Words[0] >> COMMAND_SHIFT;
    int gouraud = (command & POLYGON_GOURAUD) != 0;
    RasterState state;

    if (s_Count == 1) {
        s_Polyline[0] = vertex(word, s_Words[0]);
        s_PolylineVertices = 1;
        s_PolylineExpectsColour = gouraud;
        s_Count = 2;
    } else if ((word & POLYLINE_END_MASK) == POLYLINE_END) {
        state = draw_state(s_Words[0]);
        if (drawing_area_valid()) {
            Raster_Line(&state, s_PolylineVertices, s_Polyline);
            Render_Line(&state, s_PolylineVertices, s_Polyline);
        } else {
            /* Nothing is drawn outside a valid area */
        }
        s_Count = 0;
    } else if (s_PolylineExpectsColour) {
        s_PolylineColour = word;
        s_PolylineExpectsColour = 0;
    } else if (s_PolylineVertices < WORDS_MAX) {
        s_Polyline[s_PolylineVertices++] = vertex(word, gouraud ? s_PolylineColour : s_Words[0]);
        s_PolylineExpectsColour = gouraud;
    } else {
        Host_Fatal("GPU: a polyline longer than %d vertices", WORDS_MAX);
    }
}

void Gpu_Write(uint32_t word) {
    start();
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
