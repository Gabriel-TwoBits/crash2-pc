#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host_log.h"
#include "settings.h"
#include "world_view.h"

/* The world's polygons beside the camera's view, for the wide view (the 16:9 shape). The game draws only the polygons
 * its camera path's sort list holds at the camera's node (an SLST entry: the list at the path's first node, then each
 * node's delta from the one before), made for a 4:3 view: wider, the edges showed the background where the world
 * stopped. So the list drawn is the camera's own with the loaded worlds' polygons added that show beside the 4:3
 * picture, found by projecting their corners as the GTE will (with a margin), placed by their depth among the
 * camera's: the game draws a list from its end, the nearest first, and links each polygon no nearer than those before
 * it (link_primitive). The lists of the camera's nodes around aren't decoded for more: from a list not the node's own
 * (the camera's list and node apart), the game's decoding can loop for ever (slst_merge, a run to insert past the
 * list's end).
 * More polygons take more primitives than the game's frame buffers hold (76 of their 89 KB used on Turtle Woods, 102
 * with the polygons added): with the wide view chosen, its frame buffers are the host's own, larger, from the level's
 * opening or from the end of the frame it was chosen in. The objects come and go as the game's draw lists have them:
 * one at the edge may still come in late.
 * TRADEOFF: a polygon is placed by the sum of its corners' depths, which misorders long ones beside short ones as the
 * game's own sort does. Beside the 4:3 picture the faces turned away are drawn too (the world doesn't mark them, its
 * lists leave them out), behind the faces nearer by depth; a test of each against those drawn would leave them out.
 * A polygon cut at the camera (see the cut) takes a scratchpad slot the zone leaves free for each world cut from: in a
 * zone of 8 worlds, or past CUT_SLOTS of them, the rest are left out */

/* The sort lists: a count, a word unused, then the entries (decomp's 80039D2C.c, SORT_LIST_ENTRIES) */
#define SORT_LIST_ENTRIES 0x5F0
#define SORT_LIST_FIRST 2
/* The list drawn: the camera's and the polygons added, three lists' worth at most */
#define WIDE_LIST_ENTRIES (SORT_LIST_ENTRIES * 3)
#define ENTRY_VALUES 65536
/* An entry's world, of the zone's 8 (decomp's 80039D2C.c, ENTRY_WORLD_SHIFT) */
#define ENTRY_WORLD_SHIFT 13
#define NOWHERE 0xFFFF
#define ADDED 0xFFFE
/* A word's low bit: an EID, not a pointer */
#define EID_TAG 1
#define WORD_BYTES 4
/* The frame buffers' primitives: the PlayStation's 0x16400 bytes a frame (twice the PAL's), the host's larger */
#define HOST_PRIMITIVE_BYTES 0x40000
#define HOST_FRAME_BUFFERS 2

/* The worlds Sys_MainLoop copies into the scratchpad from 0x280, 32 bytes each (decomp's include/game/world.h,
 * WorldBlock), from the zone's first item: a count, then 12 words a world, read as Sys_MainLoop reads them (from the
 * word at 0x2C on, a world's header 6 words before it), the header counting its vertices, triangles and quads
 * (decomp's 800111B0.c) */
#define SCRATCH_WORLDS 0x280
#define WORLDS 8
#define WORLD_BLOCK_WORDS 8
#define BLOCK_OFFSET_XY 0
#define BLOCK_OFFSET_Z 1
#define BLOCK_TRIANGLES 2
#define BLOCK_QUADS 3
#define BLOCK_VERTICES 4
#define BLOCK_COLOURS 5
#define BLOCK_ANIMATIONS 6
#define BLOCK_TEXTURES 7
#define ZONE_ITEMS 0x10
#define ZONE_WORLD_FIRST 0x2C
#define ZONE_WORLD_WORDS 12
#define ZONE_WORLD_HEADER (-6)
#define HEADER_VERTICES 4
#define HEADER_TRIANGLES 5
#define HEADER_QUADS 6
/* An entry's kind and index (decomp's 80039D2C.c): a triangle, a quad's halves and the quad */
#define KIND_TRIANGLE 0
#define KIND_QUAD_FIRST 0x800
#define KIND_QUAD_LAST 0x1000
#define KIND_QUAD 0x1800
#define ENTRY_KIND_MASK 0x1800
#define ENTRY_INDEX_MASK 0x7FF
#define QUAD_RECORD_WORDS 2
/* A polygon's material: its first word's low byte and its second's low 4 bits, 0 for none; bit 11 an animation's */
#define MATERIAL_SHIFT 4
#define MATERIAL_LOW_MASK 0xF
#define MATERIAL_HIGH_MASK 0xFF
#define MATERIAL_MASK 0xFFF
#define MATERIAL_ANIMATED 0x800
#define MATERIAL_INDEX_MASK 0x7FF
/* A texture: three words, the first two a corner's u and v (low half) with the CLUT and the page (high half), the
 * third the third corner's (low) and a quad's fourth (high); an animation's word (decomp's 80039D2C.c, texture()):
 * its clock's shift, its phase, its frames less one, a texture's index or (bit 11) another animation's, or (bit 31) a
 * depth level's, 2 bits a level at a shift from a byte table by the polygon's depth */
#define TEXTURE_WORDS 3
#define HALF_LOW_MASK 0xFFFFu
#define HALF_HIGH_MASK 0xFFFF0000u
#define ANIMATION_LEVELS 0x80000000u
#define ANIMATION_CLOCK_SHIFT 26
#define ANIMATION_PHASE_SHIFT 19
#define ANIMATION_FRAMES_SHIFT 12
#define ANIMATION_FRAMES_MASK 0x7F
#define SHIFT_COUNT_MASK 0x1F
#define LEVEL_MASK 3
#define DEPTH_SUM_SHIFT 5
#define DEPTH_LEVEL_SHIFT 6
#define DEPTH_LEVELS 32
/* A vertex's colour: its index's bits from its halfword's low 4 bits, its word's low 4 and bits 16 and 17, an entry
 * of 4 bytes; a vertex's word's mode bits (18 and 19) */
#define COLOUR_HALF_MASK 0xF
#define COLOUR_HALF_SHIFT 2
#define COLOUR_HIGH_SHIFT 6
#define COLOUR_HIGH_MASK 0xC00
#define COLOUR_LOW_SHIFT 6
#define COLOUR_LOW_MASK 0x3C0
#define COLOUR_INDEX_LOW_BITS 4
#define COLOUR_INDEX_HIGH_SHIFT 8
#define COLOUR_WORD_HIGH_SHIFT 16
#define COLOUR_WORD_HIGH_MASK 3
#define VERTEX_MODE_MASK 0xC0000u
#define VERTEX_ROUND 8
#define RGB 3
#define BYTE_BITS 8
#define BYTE_MASK 0xFF
/* A polygon's vertices, 12-bit indices from its words' tops; a vertex's position bits (its word a VXY pair, its
 * halfword z) */
#define VERTEX_HIGH_SHIFT 20
#define VERTEX_LOW_SHIFT 8
#define VERTEX_HALF_SHIFT 4
#define VERTEX_INDEX_MASK 0xFFF
#define VERTEX_MASK 0xFFF0FFF0u
#define VERTEX_Z_MASK 0xFFF0
#define PAIR_MASK 0xFFFEFFFFu
#define HALF_SHIFT 16
#define TRIANGLE_CORNERS 3
#define QUAD_CORNERS 4
/* The GTE's registers in the host's memory (host.h, decomp's include/gte.h): the rotation's five words, the screen's
 * offsets (16.16) and its distance H */
extern unsigned int g_HostGte[];
#define GTE_CONTROL 32
#define GTE_RT11RT12 0
#define GTE_RT13RT21 1
#define GTE_RT22RT23 2
#define GTE_RT31RT32 3
#define GTE_RT33 4
#define GTE_OFX 24
#define GTE_OFY 25
#define GTE_H 26
#define GTE_H_MASK 0xFFFF
#define GTE_FRACTION_SHIFT 12
/* The GTE's quotient H / depth stops at 2 (its division's limit, a depth up to H / 2): a corner nearer is projected at
 * twice its offset from the screen's centre (IR1 and IR2, 16-bit), and the projection kept on the GTE's screen (SXY,
 * 11-bit) */
#define QUOTIENT_MOST 2
#define GTE_IR_MIN (-0x8000)
#define GTE_IR_MAX 0x7FFF
#define GTE_SCREEN_MIN (-0x400)
#define GTE_SCREEN_MAX 0x3FF
/* The GPU leaves out a polygon as wide as 1,024 pixels or as high as 512 */
#define GPU_POLYGON_WIDTH 1024
#define GPU_POLYGON_HEIGHT 512
/* The margin, in pixels, the polygons beside are found with past the screen's edges and inside the 4:3 picture's, so
 * that one at an edge is drawn from before it shows, whatever the GTE's rounding */
#define BESIDE_MARGIN 16
#define AXES 3
/* The wide view's narrowing (precise.c's): the 4:3 picture's half width a share of the screen's */
#define WIDE_NUMERATOR 3
#define WIDE_DENOMINATOR 4
extern unsigned char g_HostScratchpad[];
extern unsigned char *g_CurrentZone;
/* The textures' clock and the depth levels' byte table (decomp's Gfx_DrawWorlds' arguments and 80039D2C.c) */
extern int32_t D_80060944;
extern uint8_t D_8005C724[];

/* The game's (decomp's include/game.h, 80026CA0.c) */
int32_t Heap_Alloc(int32_t bytes);
void Heap_Free(int32_t buffer);
extern int32_t g_CameraPath;

static unsigned char s_Primitives[HOST_FRAME_BUFFERS][HOST_PRIMITIVE_BYTES] __attribute__((aligned(16)));
static int s_PrimitivesTaken[HOST_FRAME_BUFFERS];
/* Whether the frame buffers in use are the host's (the wide view chosen when the level opened, or since) */
static int s_LargeBuffers;

static uint16_t s_Wide[SORT_LIST_FIRST + WIDE_LIST_ENTRIES];
/* An entry's place in the camera's list (NOWHERE if not there, ADDED if added already), and the entries added with
 * the camera's entry each follows */
static uint16_t s_Place[ENTRY_VALUES];
static uint16_t s_Added[WIDE_LIST_ENTRIES];
static uint16_t s_AddedAfter[WIDE_LIST_ENTRIES];
/* An added entry's depth: the sum of the depths of the corners the game's depth slot sums */
static int32_t s_AddedDepth[WIDE_LIST_ENTRIES];
static int s_AddedCount;
static int s_Order[WIDE_LIST_ENTRIES];
/* The camera's entries' depths, and the most of each from it to the list's end */
static int32_t s_CameraDepth[SORT_LIST_ENTRIES];
static int32_t s_DepthOnward[SORT_LIST_ENTRIES];
/* The zone's worlds, as the GTE turns them this frame */
static const int32_t *s_Zone;
static int s_WorldCount;
static int32_t s_Rotation[AXES][AXES];

/* An added entry's slot: 0 before the camera's entries, k + 1 after the camera's k-th */
static int slot_after(uint16_t after) {
    if (after == NOWHERE) {
        return 0;
    } else {
        return after + 1;
    }
}

static int wide(void) {
    return g_Settings[SETTING_SHAPE].value == SHAPE_WIDE;
}

int32_t Host_FramePrimitivesAlloc(int32_t bytes) {
    int index;

    if (!wide()) {
        s_LargeBuffers = 0;
        return Heap_Alloc(bytes);
    } else {
        s_LargeBuffers = 1;
    }
    for (index = 0; index < HOST_FRAME_BUFFERS; index++) {
        if (!s_PrimitivesTaken[index]) {
            s_PrimitivesTaken[index] = 1;
            return (int32_t)(uintptr_t)s_Primitives[index];
        } else {
            continue;
        }
    }
    Host_Fatal("the wide view: more than %d frame buffers", HOST_FRAME_BUFFERS);
    return 0;
}

void Host_FramePrimitivesFree(int32_t buffer) {
    int index;

    for (index = 0; index < HOST_FRAME_BUFFERS; index++) {
        if (buffer == (int32_t)(uintptr_t)s_Primitives[index]) {
            s_PrimitivesTaken[index] = 0;
            return;
        } else {
            continue;
        }
    }
    Heap_Free(buffer);
}

/* A frame buffer's primitives (decomp's 800158E0.c, func_800163C4): their start, and where the next goes */
#define FRAME_PRIMITIVES 1
#define FRAME_PRIMITIVES_NEXT 2
extern unsigned char *g_BuildBuffer[];
extern unsigned char *g_DrawBuffer;

static void take_large(unsigned char *frame_bytes) {
    int32_t *frame = (int32_t *)frame_bytes;
    int32_t buffer = frame[FRAME_PRIMITIVES];

    frame[FRAME_PRIMITIVES] = Host_FramePrimitivesAlloc(HOST_PRIMITIVE_BYTES);
    frame[FRAME_PRIMITIVES_NEXT] = frame[FRAME_PRIMITIVES];
    Heap_Free(buffer);
}

/* The wide view chosen while a level is open: its frame buffers the host's from here, where the frame drawn has gone
 * to the GPU and the frame being built is empty (decomp's Gfx_EndFrame, FRAME_ENDED) */
void WorldView_FrameEnded(void) {
    if (!wide() || s_LargeBuffers || g_BuildBuffer[0] == NULL || g_DrawBuffer == NULL ||
        ((int32_t *)g_BuildBuffer[0])[FRAME_PRIMITIVES] == 0 || ((int32_t *)g_DrawBuffer)[FRAME_PRIMITIVES] == 0) {
        return;
    } else {
        take_large(g_BuildBuffer[0]);
        take_large(g_DrawBuffer);
    }
}

/* Added entries in the list drawn's order: by the camera's entry each follows, then the farther first */
static int added_order(const void *left_item, const void *right_item) {
    int left = *(const int *)left_item;
    int right = *(const int *)right_item;
    int left_slot = slot_after(s_AddedAfter[left]);
    int right_slot = slot_after(s_AddedAfter[right]);

    if (left_slot != right_slot) {
        return left_slot < right_slot ? -1 : 1;
    } else if (s_AddedDepth[left] != s_AddedDepth[right]) {
        return s_AddedDepth[left] > s_AddedDepth[right] ? -1 : 1;
    } else {
        return left - right;
    }
}

/* The list drawn: the entries added before the camera's first, then each of the camera's followed by those added
 * after it */
static void write_wide(const uint16_t *camera, int count) {
    int written = 0;
    int next = 0;
    int index;

    for (index = 0; index < s_AddedCount; index++) {
        s_Order[index] = index;
    }
    qsort(s_Order, (size_t)s_AddedCount, sizeof(s_Order[0]), added_order);
    for (index = 0; index <= count; index++) {
        if (index > 0) {
            s_Wide[SORT_LIST_FIRST + written++] = camera[SORT_LIST_FIRST + index - 1];
        } else {
            /* The slot before the camera's entries */
        }
        while (next < s_AddedCount && slot_after(s_AddedAfter[s_Order[next]]) == index) {
            s_Wide[SORT_LIST_FIRST + written++] = s_Added[s_Order[next++]];
        }
    }
    s_Wide[0] = (uint16_t)written;
    s_Wide[1] = camera[1];
}

/* The zone's worlds and the GTE's rotation, as Gfx_DrawWorlds will find them */
static void take_scene(void) {
    const unsigned int *control = &g_HostGte[GTE_CONTROL];

    s_Zone = *(const int32_t *const *)(g_CurrentZone + ZONE_ITEMS);
    s_WorldCount = s_Zone[0] < WORLDS ? s_Zone[0] : WORLDS;
    s_Rotation[0][0] = (int16_t)control[GTE_RT11RT12];
    s_Rotation[0][1] = (int16_t)(control[GTE_RT11RT12] >> HALF_SHIFT);
    s_Rotation[0][2] = (int16_t)control[GTE_RT13RT21];
    s_Rotation[1][0] = (int16_t)(control[GTE_RT13RT21] >> HALF_SHIFT);
    s_Rotation[1][1] = (int16_t)control[GTE_RT22RT23];
    s_Rotation[1][2] = (int16_t)(control[GTE_RT22RT23] >> HALF_SHIFT);
    s_Rotation[2][0] = (int16_t)control[GTE_RT31RT32];
    s_Rotation[2][1] = (int16_t)(control[GTE_RT31RT32] >> HALF_SHIFT);
    s_Rotation[2][2] = (int16_t)control[GTE_RT33];
}

/* A world's header, NULL if it isn't loaded */
static const int32_t *world_header(int world) {
    const int32_t *header;

    if (world >= s_WorldCount) {
        return NULL;
    } else {
        header = (const int32_t *)(uintptr_t)s_Zone[ZONE_WORLD_FIRST / WORD_BYTES + ZONE_WORLD_WORDS * world +
                                                    ZONE_WORLD_HEADER];
    }
    if (header == NULL || ((uintptr_t)header & EID_TAG)) {
        return NULL;
    } else {
        return header;
    }
}

static const uint32_t *world_block(int world) {
    return (const uint32_t *)(g_HostScratchpad + SCRATCH_WORLDS) + world * WORLD_BLOCK_WORDS;
}

/* An entry's corners as Gfx_DrawWorlds draws them (a triangle's three, a quad's four, a quad's half's three); their
 * count, 0 for an index past its world's table */
static int entry_corners(uint16_t entry, const int32_t *header, uint32_t *corners) {
    const uint32_t *block = world_block(entry >> ENTRY_WORLD_SHIFT);
    uint32_t kind = entry & ENTRY_KIND_MASK;
    uint32_t index = entry & ENTRY_INDEX_MASK;

    if (kind == KIND_TRIANGLE && index < (uint32_t)header[HEADER_TRIANGLES]) {
        const unsigned char *triangles = (const unsigned char *)(uintptr_t)block[BLOCK_TRIANGLES];
        uint32_t first = *(const uint32_t *)(triangles - (index + 1) * WORD_BYTES);
        uint32_t second = *(const uint16_t *)(triangles + index * sizeof(uint16_t));

        corners[0] = first >> VERTEX_HIGH_SHIFT;
        corners[1] = (first >> VERTEX_LOW_SHIFT) & VERTEX_INDEX_MASK;
        corners[2] = (second >> VERTEX_HALF_SHIFT) & VERTEX_INDEX_MASK;
        return TRIANGLE_CORNERS;
    } else if (kind != KIND_TRIANGLE && index < (uint32_t)header[HEADER_QUADS]) {
        const uint32_t *quads = (const uint32_t *)(uintptr_t)block[BLOCK_QUADS];
        uint32_t first = quads[index * QUAD_RECORD_WORDS];
        uint32_t second = quads[index * QUAD_RECORD_WORDS + 1];
        int corner;

        corners[0] = first >> VERTEX_HIGH_SHIFT;
        corners[1] = (first >> VERTEX_LOW_SHIFT) & VERTEX_INDEX_MASK;
        corners[2] = second >> VERTEX_HIGH_SHIFT;
        corners[3] = (second >> VERTEX_LOW_SHIFT) & VERTEX_INDEX_MASK;
        if (kind == KIND_QUAD) {
            return QUAD_CORNERS;
        } else if (kind == KIND_QUAD_FIRST) {
            return TRIANGLE_CORNERS;
        } else {
            /* The last three corners' triangle */
            for (corner = 0; corner < TRIANGLE_CORNERS; corner++) {
                corners[corner] = corners[corner + 1];
            }
            return TRIANGLE_CORNERS;
        }
    } else {
        return 0;
    }
}

static int64_t clamped(int64_t value, int64_t low, int64_t high) {
    if (value < low) {
        return low;
    } else if (value > high) {
        return high;
    } else {
        return value;
    }
}

/* A polygon as the camera sees it: its corners in the order around it, each's position from the camera before and
 * after the GTE's rotation, and where the GTE puts it on the screen (from its centre, narrowed); its depth (the sum of
 * those of the corners the game's depth slot sums) and its bounds on the screen */
typedef struct {
    int world;
    uint32_t first, second;
    int count;
    uint32_t corners[QUAD_CORNERS];
    int64_t model[QUAD_CORNERS][AXES];
    int64_t view[QUAD_CORNERS][AXES];
    int32_t depth;
    int64_t low_x, high_x, low_y, high_y;
    /* A corner nearer than the GTE's division's limit or behind the camera, or bounds the GPU leaves out */
    int cut;
} Seen;

/* A world's vertex: its position from the camera (the GTE's VXY pair and VZ, as Gfx_DrawWorlds loads it), and as the
 * camera sees it, turned by the GTE's rotation */
static void view_vertex(const uint32_t *world, uint32_t index, int64_t *model, int64_t *view) {
    const unsigned char *vertices = (const unsigned char *)(uintptr_t)world[BLOCK_VERTICES];
    uint32_t word = *(const uint32_t *)(vertices - (index + 1) * WORD_BYTES);
    uint32_t half = *(const uint16_t *)(vertices + index * sizeof(uint16_t));
    uint32_t pair = ((word & VERTEX_MASK) + world[BLOCK_OFFSET_XY]) & PAIR_MASK;
    int axis;

    model[0] = (int16_t)pair;
    model[1] = (int16_t)(pair >> HALF_SHIFT);
    model[2] = (int16_t)((half & VERTEX_Z_MASK) + world[BLOCK_OFFSET_Z]);
    for (axis = 0; axis < AXES; axis++) {
        view[axis] = (s_Rotation[axis][0] * model[0] + s_Rotation[axis][1] * model[1] +
                      s_Rotation[axis][2] * model[2]) >> GTE_FRACTION_SHIFT;
    }
}

/* Where the GTE puts a corner on the screen, from its centre (with the wide view's narrowing), kept on its screen */
static void project(const int64_t *view, int64_t *x, int64_t *y) {
    int64_t h = g_HostGte[GTE_CONTROL + GTE_H] & GTE_H_MASK;
    int64_t half_width = (int64_t)(g_HostGte[GTE_CONTROL + GTE_OFX] >> HALF_SHIFT);
    int64_t half_height = (int64_t)(g_HostGte[GTE_CONTROL + GTE_OFY] >> HALF_SHIFT);

    if (view[2] * QUOTIENT_MOST <= h) {
        *x = clamped(view[0], GTE_IR_MIN, GTE_IR_MAX) * QUOTIENT_MOST * WIDE_NUMERATOR / WIDE_DENOMINATOR;
        *y = clamped(view[1], GTE_IR_MIN, GTE_IR_MAX) * QUOTIENT_MOST;
    } else {
        *x = h * view[0] / view[2] * WIDE_NUMERATOR / WIDE_DENOMINATOR;
        *y = h * view[1] / view[2];
    }
    *x = clamped(*x, GTE_SCREEN_MIN - half_width, GTE_SCREEN_MAX - half_width);
    *y = clamped(*y, GTE_SCREEN_MIN - half_height, GTE_SCREEN_MAX - half_height);
}

/* The polygon of an entry as the camera sees it; 0 if its world isn't loaded or its tables don't hold it */
static int see(uint16_t entry, Seen *seen) {
    const uint32_t *block;
    const int32_t *header;
    uint32_t kind = entry & ENTRY_KIND_MASK;
    uint32_t index = entry & ENTRY_INDEX_MASK;
    int64_t h = g_HostGte[GTE_CONTROL + GTE_H] & GTE_H_MASK;
    int corner;

    seen->world = entry >> ENTRY_WORLD_SHIFT;
    header = world_header(seen->world);
    block = world_block(seen->world);
    seen->count = header == NULL ? 0 : entry_corners(entry, header, seen->corners);
    if (seen->count == 0) {
        return 0;
    } else if (kind == KIND_TRIANGLE) {
        const unsigned char *triangles = (const unsigned char *)(uintptr_t)block[BLOCK_TRIANGLES];

        seen->first = *(const uint32_t *)(triangles - (index + 1) * WORD_BYTES);
        seen->second = *(const uint16_t *)(triangles + index * sizeof(uint16_t));
    } else {
        const uint32_t *quads = (const uint32_t *)(uintptr_t)block[BLOCK_QUADS];

        seen->first = quads[index * QUAD_RECORD_WORDS];
        seen->second = quads[index * QUAD_RECORD_WORDS + 1];
    }
    seen->depth = 0;
    seen->cut = 0;
    seen->low_x = INT64_MAX;
    seen->high_x = INT64_MIN;
    seen->low_y = INT64_MAX;
    seen->high_y = INT64_MIN;
    for (corner = 0; corner < seen->count; corner++) {
        int64_t x, y;

        if (seen->corners[corner] >= (uint32_t)header[HEADER_VERTICES]) {
            return 0;
        } else {
            view_vertex(block, seen->corners[corner], seen->model[corner], seen->view[corner]);
        }
        if (corner >= seen->count - TRIANGLE_CORNERS && seen->view[corner][2] > 0) {
            seen->depth += (int32_t)seen->view[corner][2];
        } else {
            /* A quad's first corner, which the game's depth slot leaves out, or a depth it takes as 0 */
        }
        seen->cut |= seen->view[corner][2] * QUOTIENT_MOST <= h;
        project(seen->view[corner], &x, &y);
        seen->low_x = x < seen->low_x ? x : seen->low_x;
        seen->high_x = x > seen->high_x ? x : seen->high_x;
        seen->low_y = y < seen->low_y ? y : seen->low_y;
        seen->high_y = y > seen->high_y ? y : seen->high_y;
    }
    seen->cut |= seen->high_x - seen->low_x >= GPU_POLYGON_WIDTH || seen->high_y - seen->low_y >= GPU_POLYGON_HEIGHT;
    return 1;
}

/* Whether bounds on the screen show beside the 4:3 picture (past its edge on one side and on the screen, both with
 * the margin), and whether they lie wholly outside it */
static int bounds_beside(int64_t low_x, int64_t high_x, int64_t low_y, int64_t high_y, int *outside) {
    int64_t half_width = (int64_t)(g_HostGte[GTE_CONTROL + GTE_OFX] >> HALF_SHIFT);
    int64_t half_height = (int64_t)(g_HostGte[GTE_CONTROL + GTE_OFY] >> HALF_SHIFT);
    int64_t band = half_width * WIDE_NUMERATOR / WIDE_DENOMINATOR;

    *outside = low_x > band || high_x < -band;
    return high_x > -half_width - BESIDE_MARGIN && low_x < half_width + BESIDE_MARGIN &&
           high_y > -half_height - BESIDE_MARGIN && low_y < half_height + BESIDE_MARGIN &&
           (high_x > band - BESIDE_MARGIN || low_x < BESIDE_MARGIN - band);
}

/* The cut: a polygon beside reaching nearer than the GTE's division's limit, behind the camera or across more than
 * the GPU draws is cut at a plane before the camera (CUT_NEAR_MARGIN past that limit, H / 2, more than a new corner's
 * rounding moves it), at four planes beyond the screen's edges (CUT_GUARD pixels out, which keep a piece within what
 * the GPU draws) and at the 4:3 picture's edge, a piece on each side, and its pieces drawn as triangles of a world the
 * host makes: the GTE then projects each corner right. The 4:3 picture stays as the game draws it: the camera's own
 * polygons are left as they are (cut and drawn whole, the trees and walls against the camera that the game's lists
 * keep but the PlayStation never drew showed, their textures stretched across the screen: tried). A host world is
 * one of the scratchpad's slots the zone leaves free, one for each world cut from, its position from the camera that
 * world's (so a corner kept keeps its exact position, a new one rounded to the 16 units of a vertex's), its own
 * vertices, triangles, colours (a new corner's between its edge's two) and textures (a new corner's u and v between
 * its edge's two). An animated texture is taken at the clock's frame, a depth level's at the polygon's depth */
#define CUT_NEAR_MARGIN 32
#define CUT_GUARD 128
#define CUT_PLANES 6
#define CUT_CORNERS_MOST (QUAD_CORNERS + CUT_PLANES)
#define CUT_SLOTS 4
#define CUT_VERTICES 1024
#define CUT_TRIANGLES 512
#define CUT_COLOURS 1024
#define CUT_TEXTURES 512
/* A vertex's and a triangle's records: a word below their table's address and a halfword above */
#define RECORD_BYTES (WORD_BYTES + sizeof(uint16_t))
#define NO_SLOT (-1)

enum { PLANE_NEAR, PLANE_LEFT, PLANE_RIGHT, PLANE_TOP, PLANE_BOTTOM, PLANE_BESIDE_LEFT, PLANE_BESIDE_RIGHT };

typedef struct {
    double model[AXES];
    double view[AXES];
    double u, v;
    double colour[RGB];
    uint32_t colour_top;
    uint32_t mode;
} CutCorner;

typedef struct {
    int source;
    int vertices, triangles, colours, textures;
    unsigned char vertex_table[CUT_VERTICES * RECORD_BYTES];
    unsigned char triangle_table[CUT_TRIANGLES * RECORD_BYTES];
    uint32_t colour_table[CUT_COLOURS];
    uint32_t texture_table[CUT_TEXTURES * TEXTURE_WORDS];
} CutWorld;

static CutWorld s_Cut[CUT_SLOTS];
static int s_CutCount;
/* The host world a world's pieces go into, NO_SLOT if none yet */
static int s_CutOf[WORLDS];

static void cut_begin(void) {
    int world;

    s_CutCount = 0;
    for (world = 0; world < WORLDS; world++) {
        s_CutOf[world] = NO_SLOT;
    }
}

/* The scratchpad's slot of a host world */
static int cut_slot(int cut) {
    return s_WorldCount + cut;
}

/* The host world for a world's pieces, NO_SLOT if the scratchpad has no slot left */
static int cut_world(int world) {
    CutWorld *cut;

    if (s_CutOf[world] != NO_SLOT) {
        return s_CutOf[world];
    } else if (s_CutCount >= CUT_SLOTS || cut_slot(s_CutCount) >= WORLDS) {
        return NO_SLOT;
    } else {
        cut = &s_Cut[s_CutCount];
        cut->source = world;
        cut->vertices = 0;
        cut->triangles = 0;
        cut->colours = 0;
        cut->textures = 0;
        s_CutOf[world] = s_CutCount++;
        return s_CutOf[world];
    }
}

/* The host worlds into the scratchpad's slots, as Sys_MainLoop puts the zone's */
static void cut_end(void) {
    int cut;

    for (cut = 0; cut < s_CutCount; cut++) {
        const uint32_t *source = world_block(s_Cut[cut].source);
        uint32_t *block = (uint32_t *)(g_HostScratchpad + SCRATCH_WORLDS) + cut_slot(cut) * WORLD_BLOCK_WORDS;

        block[BLOCK_OFFSET_XY] = source[BLOCK_OFFSET_XY];
        block[BLOCK_OFFSET_Z] = source[BLOCK_OFFSET_Z];
        block[BLOCK_TRIANGLES] = (uint32_t)(uintptr_t)(s_Cut[cut].triangle_table + CUT_TRIANGLES * WORD_BYTES);
        block[BLOCK_QUADS] = source[BLOCK_QUADS];
        block[BLOCK_VERTICES] = (uint32_t)(uintptr_t)(s_Cut[cut].vertex_table + CUT_VERTICES * WORD_BYTES);
        block[BLOCK_COLOURS] = (uint32_t)(uintptr_t)s_Cut[cut].colour_table;
        block[BLOCK_ANIMATIONS] = source[BLOCK_ANIMATIONS];
        block[BLOCK_TEXTURES] = (uint32_t)(uintptr_t)s_Cut[cut].texture_table;
    }
}

/* A material's texture's three words as the renderer takes them at a depth (decomp's 80039D2C.c, texture()) */
static const uint32_t *texture_words(const uint32_t *block, uint32_t material, int32_t depth) {
    const uint32_t *animations = (const uint32_t *)(uintptr_t)block[BLOCK_ANIMATIONS];
    const uint32_t *textures = (const uint32_t *)(uintptr_t)block[BLOCK_TEXTURES];
    uint32_t index = material & MATERIAL_INDEX_MASK;
    uint32_t level = ((uint32_t)depth >> DEPTH_SUM_SHIFT) >> DEPTH_LEVEL_SHIFT;
    uint32_t word, frame, shift;

    if (!(material & MATERIAL_ANIMATED)) {
        return textures + (index - 1) * TEXTURE_WORDS;
    } else {
        word = animations[index];
        shift = D_8005C724[level < DEPTH_LEVELS ? level : DEPTH_LEVELS - 1];
    }
    if (!(word & ANIMATION_LEVELS)) {
        frame = (((uint32_t)D_80060944 >> ((word >> ANIMATION_CLOCK_SHIFT) & SHIFT_COUNT_MASK)) +
                 (word >> ANIMATION_PHASE_SHIFT)) & ((word >> ANIMATION_FRAMES_SHIFT) & ANIMATION_FRAMES_MASK);
        index = (word + frame) & MATERIAL_INDEX_MASK;
        if (word & MATERIAL_ANIMATED) {
            word = animations[index];
        } else {
            return textures + (index - 1) * TEXTURE_WORDS;
        }
    } else {
        /* A depth level's */
    }
    index = (word & MATERIAL_INDEX_MASK) + ((word >> (shift & SHIFT_COUNT_MASK)) & LEVEL_MASK);
    return textures + (index - 1) * TEXTURE_WORDS;
}

/* A vertex's colour's index in its world's colour table (decomp's 80039D2C.c, vertex_colour()) */
static uint32_t colour_index(uint32_t word, uint32_t half) {
    uint32_t offset = ((half & COLOUR_HALF_MASK) << COLOUR_HALF_SHIFT) |
                      ((word >> COLOUR_HIGH_SHIFT) & COLOUR_HIGH_MASK) | ((word << COLOUR_LOW_SHIFT) & COLOUR_LOW_MASK);

    return offset / WORD_BYTES;
}

/* A seen polygon's corners, with their textures' u and v and their colours, in the order around it */
static int cut_corners(const Seen *seen, uint16_t entry, CutCorner *out) {
    static const int s_QuadAround[QUAD_CORNERS] = {0, 1, 3, 2};
    const uint32_t *block = world_block(seen->world);
    const unsigned char *vertices = (const unsigned char *)(uintptr_t)block[BLOCK_VERTICES];
    const uint32_t *colours = (const uint32_t *)(uintptr_t)block[BLOCK_COLOURS];
    uint32_t material = ((seen->first << MATERIAL_SHIFT) | (seen->second & MATERIAL_LOW_MASK)) & MATERIAL_MASK;
    uint32_t kind = entry & ENTRY_KIND_MASK;
    uint32_t uv[QUAD_CORNERS] = {0, 0, 0, 0};
    int corner;

    if (material != 0) {
        const uint32_t *words = texture_words(block, material, seen->depth);
        uint32_t all[QUAD_CORNERS];
        int first_uv = kind == KIND_QUAD_LAST ? 1 : 0;

        all[0] = words[0] & HALF_LOW_MASK;
        all[1] = words[1] & HALF_LOW_MASK;
        all[2] = words[2] & HALF_LOW_MASK;
        all[3] = words[2] >> HALF_SHIFT;
        for (corner = 0; corner < seen->count; corner++) {
            uv[corner] = all[first_uv + corner];
        }
    } else {
        /* Shaded only */
    }
    for (corner = 0; corner < seen->count; corner++) {
        int at = seen->count == QUAD_CORNERS ? s_QuadAround[corner] : corner;
        uint32_t index = seen->corners[at];
        uint32_t word = *(const uint32_t *)(vertices - (index + 1) * WORD_BYTES);
        uint32_t half = *(const uint16_t *)(vertices + index * sizeof(uint16_t));
        uint32_t colour = colours[colour_index(word, half)];
        int axis;

        for (axis = 0; axis < AXES; axis++) {
            out[corner].model[axis] = (double)seen->model[at][axis];
            out[corner].view[axis] = (double)seen->view[at][axis];
        }
        out[corner].u = (double)(uv[at] & BYTE_MASK);
        out[corner].v = (double)(uv[at] >> BYTE_BITS);
        for (axis = 0; axis < RGB; axis++) {
            out[corner].colour[axis] = (double)((colour >> (axis * BYTE_BITS)) & BYTE_MASK);
        }
        out[corner].colour_top = colour & ~((1u << (RGB * BYTE_BITS)) - 1);
        out[corner].mode = word & VERTEX_MODE_MASK;
    }
    return seen->count;
}

/* How far inside a plane a corner is (none below 0 inside) */
static double plane_distance(const CutCorner *corner, int plane) {
    double h = (double)(g_HostGte[GTE_CONTROL + GTE_H] & GTE_H_MASK);
    double guard_x = (double)((g_HostGte[GTE_CONTROL + GTE_OFX] >> HALF_SHIFT) + CUT_GUARD);
    double guard_y = (double)((g_HostGte[GTE_CONTROL + GTE_OFY] >> HALF_SHIFT) + CUT_GUARD);
    double across = h * WIDE_NUMERATOR / WIDE_DENOMINATOR;
    double band = (double)((g_HostGte[GTE_CONTROL + GTE_OFX] >> HALF_SHIFT) * WIDE_NUMERATOR / WIDE_DENOMINATOR);

    switch (plane) {
    case PLANE_NEAR:
        return corner->view[2] - h / QUOTIENT_MOST - CUT_NEAR_MARGIN;
    case PLANE_LEFT:
        return guard_x * corner->view[2] + across * corner->view[0];
    case PLANE_RIGHT:
        return guard_x * corner->view[2] - across * corner->view[0];
    case PLANE_TOP:
        return guard_y * corner->view[2] + h * corner->view[1];
    case PLANE_BOTTOM:
        return guard_y * corner->view[2] - h * corner->view[1];
    case PLANE_BESIDE_LEFT:
        return -(band * corner->view[2] + across * corner->view[0]);
    case PLANE_BESIDE_RIGHT:
        return across * corner->view[0] - band * corner->view[2];
    default:
        Host_Fatal("the wide view: plane %d", plane);
        return 0;
    }
}

static CutCorner between(const CutCorner *from, const CutCorner *to, double share) {
    CutCorner corner;
    int axis;

    for (axis = 0; axis < AXES; axis++) {
        corner.model[axis] = from->model[axis] + (to->model[axis] - from->model[axis]) * share;
        corner.view[axis] = from->view[axis] + (to->view[axis] - from->view[axis]) * share;
    }
    corner.u = from->u + (to->u - from->u) * share;
    corner.v = from->v + (to->v - from->v) * share;
    for (axis = 0; axis < RGB; axis++) {
        corner.colour[axis] = from->colour[axis] + (to->colour[axis] - from->colour[axis]) * share;
    }
    corner.colour_top = from->colour_top;
    corner.mode = from->mode;
    return corner;
}

/* A polygon cut at a plane (Sutherland and Hodgman's): its corners inside, and where its edges cross */
static int cut_at(const CutCorner *in, int count, int plane, CutCorner *out) {
    int written = 0;
    int corner;

    for (corner = 0; corner < count; corner++) {
        const CutCorner *from = &in[corner];
        const CutCorner *to = &in[(corner + 1) % count];
        double from_distance = plane_distance(from, plane);
        double to_distance = plane_distance(to, plane);

        if (from_distance >= 0) {
            out[written++] = *from;
        } else {
            /* Outside */
        }
        if ((from_distance >= 0) != (to_distance >= 0)) {
            out[written++] = between(from, to, from_distance / (from_distance - to_distance));
        } else {
            /* The edge on one side */
        }
    }
    return written;
}

static uint32_t rounded_position(double value) {
    int32_t whole = (int32_t)(value < 0 ? value - 0.5 : value + 0.5);

    return (uint32_t)(whole + VERTEX_ROUND) & VERTEX_Z_MASK;
}

static int byte_of(double value) {
    int whole = (int)(value + 0.5);

    return (int)clamped(whole, 0, BYTE_MASK);
}

/* A corner as a vertex of a host world, its colour its own; its index, -1 if the tables are full */
static int cut_vertex(CutWorld *cut, const CutCorner *corner) {
    const uint32_t *source = world_block(cut->source);
    uint32_t offset_x = (int16_t)source[BLOCK_OFFSET_XY];
    uint32_t offset_y = (int16_t)(source[BLOCK_OFFSET_XY] >> HALF_SHIFT);
    uint32_t offset_z = source[BLOCK_OFFSET_Z];
    uint32_t colour = corner->colour_top;
    uint32_t x, y, z, word, half;
    int axis;

    if (cut->vertices >= CUT_VERTICES || cut->colours >= CUT_COLOURS) {
        return -1;
    } else {
        x = rounded_position(corner->model[0] - (double)(int32_t)offset_x);
        y = rounded_position(corner->model[1] - (double)(int32_t)offset_y);
        z = rounded_position(corner->model[2] - (double)(int32_t)offset_z);
    }
    for (axis = 0; axis < RGB; axis++) {
        colour |= (uint32_t)byte_of(corner->colour[axis]) << (axis * BYTE_BITS);
    }
    cut->colour_table[cut->colours] = colour;
    word = (y << HALF_SHIFT) | x | corner->mode |
           (((uint32_t)cut->colours >> COLOUR_INDEX_LOW_BITS) & COLOUR_HALF_MASK) |
           ((((uint32_t)cut->colours >> COLOUR_INDEX_HIGH_SHIFT) & COLOUR_WORD_HIGH_MASK) << COLOUR_WORD_HIGH_SHIFT);
    half = z | ((uint32_t)cut->colours & COLOUR_HALF_MASK);
    cut->colours++;
    *(uint32_t *)(cut->vertex_table + (CUT_VERTICES - cut->vertices - 1) * WORD_BYTES) = word;
    *(uint16_t *)(cut->vertex_table + CUT_VERTICES * WORD_BYTES + cut->vertices * sizeof(uint16_t)) = (uint16_t)half;
    return cut->vertices++;
}

static uint32_t uv_of(const CutCorner *corner) {
    return ((uint32_t)byte_of(corner->v) << BYTE_BITS) | (uint32_t)byte_of(corner->u);
}

/* A triangle of a host world from three of a piece's corners, its texture the polygon's with their u and v; its
 * entry, NOWHERE if the tables are full */
static uint16_t cut_triangle(int slot, const CutCorner *a, const CutCorner *b, const CutCorner *c,
                             const uint32_t *words) {
    CutWorld *cut = &s_Cut[slot];
    int corner_a = cut_vertex(cut, a);
    int corner_b = cut_vertex(cut, b);
    int corner_c = cut_vertex(cut, c);
    uint32_t material = 0;
    uint32_t first, second;

    if (corner_a < 0 || corner_b < 0 || corner_c < 0 || cut->triangles >= CUT_TRIANGLES) {
        return NOWHERE;
    } else if (words != NULL && cut->textures < CUT_TEXTURES) {
        uint32_t *texture = &cut->texture_table[cut->textures * TEXTURE_WORDS];

        texture[0] = (words[0] & HALF_HIGH_MASK) | uv_of(a);
        texture[1] = (words[1] & HALF_HIGH_MASK) | uv_of(b);
        texture[2] = uv_of(c);
        material = (uint32_t)++cut->textures;
    } else if (words != NULL) {
        return NOWHERE;
    } else {
        /* Shaded only */
    }
    first = ((uint32_t)corner_a << VERTEX_HIGH_SHIFT) | ((uint32_t)corner_b << VERTEX_LOW_SHIFT) |
            ((material >> MATERIAL_SHIFT) & MATERIAL_HIGH_MASK);
    second = ((uint32_t)corner_c << VERTEX_HALF_SHIFT) | (material & MATERIAL_LOW_MASK);
    *(uint32_t *)(cut->triangle_table + (CUT_TRIANGLES - cut->triangles - 1) * WORD_BYTES) = first;
    *(uint16_t *)(cut->triangle_table + CUT_TRIANGLES * WORD_BYTES + cut->triangles * sizeof(uint16_t)) =
        (uint16_t)second;
    return (uint16_t)(((uint32_t)cut_slot(slot) << ENTRY_WORLD_SHIFT) | KIND_TRIANGLE | (uint32_t)cut->triangles++);
}

/* A seen polygon cut, beside the 4:3 picture on one side (PLANE_BESIDE_LEFT or PLANE_BESIDE_RIGHT): its piece's
 * corners and their count (0 if none is left), and the piece's bounds on the screen */
static int cut_polygon(const Seen *seen, uint16_t entry, int side, CutCorner *piece, int64_t *bounds) {
    CutCorner other[CUT_CORNERS_MOST * 2];
    int count = cut_corners(seen, entry, piece);
    int plane, corner;

    for (plane = PLANE_NEAR; plane <= PLANE_BOTTOM && count > 0; plane++) {
        count = cut_at(piece, count, plane, other);
        memcpy(piece, other, sizeof(other[0]) * (size_t)count);
    }
    if (count > 0) {
        count = cut_at(piece, count, side, other);
        memcpy(piece, other, sizeof(other[0]) * (size_t)count);
    } else {
        /* Nothing left */
    }
    bounds[0] = INT64_MAX;
    bounds[1] = INT64_MIN;
    bounds[2] = INT64_MAX;
    bounds[3] = INT64_MIN;
    for (corner = 0; corner < count; corner++) {
        int64_t view[AXES];
        int64_t x, y;
        int axis;

        for (axis = 0; axis < AXES; axis++) {
            view[axis] = (int64_t)piece[corner].view[axis];
        }
        project(view, &x, &y);
        bounds[0] = x < bounds[0] ? x : bounds[0];
        bounds[1] = x > bounds[1] ? x : bounds[1];
        bounds[2] = y < bounds[2] ? y : bounds[2];
        bounds[3] = y > bounds[3] ? y : bounds[3];
    }
    return count < TRIANGLE_CORNERS ? 0 : count;
}

/* A piece's triangles added, each after the camera's entry `after` with the polygon's depth; how many */
static int add_piece(const Seen *seen, const CutCorner *piece, int count, uint16_t after) {
    const uint32_t *block = world_block(seen->world);
    uint32_t material = ((seen->first << MATERIAL_SHIFT) | (seen->second & MATERIAL_LOW_MASK)) & MATERIAL_MASK;
    const uint32_t *words = material == 0 ? NULL : texture_words(block, material, seen->depth);
    int slot = cut_world(seen->world);
    int added = 0;
    int corner;

    if (slot == NO_SLOT) {
        return 0;
    } else {
        /* A fan from the first corner */
    }
    for (corner = 1; corner + 1 < count && s_AddedCount < WIDE_LIST_ENTRIES - SORT_LIST_ENTRIES; corner++) {
        uint16_t entry = cut_triangle(slot, &piece[0], &piece[corner], &piece[corner + 1], words);

        if (entry == NOWHERE) {
            break;
        } else {
            s_Added[s_AddedCount] = entry;
            s_AddedAfter[s_AddedCount] = after;
            s_AddedDepth[s_AddedCount] = seen->depth;
            s_AddedCount++;
            added++;
        }
    }
    return added;
}

/* The camera's entries' depths, and the deepest of each from it on */
static void camera_depths(const uint16_t *camera, int count) {
    int index;

    for (index = 0; index < count; index++) {
        Seen seen;

        if (see(camera[SORT_LIST_FIRST + index], &seen)) {
            s_CameraDepth[index] = seen.depth;
        } else {
            s_CameraDepth[index] = 0;
        }
    }
    for (index = count - 1; index >= 0; index--) {
        if (index == count - 1 || s_CameraDepth[index] > s_DepthOnward[index + 1]) {
            s_DepthOnward[index] = s_CameraDepth[index];
        } else {
            s_DepthOnward[index] = s_DepthOnward[index + 1];
        }
    }
}

/* The camera's entry an entry of this depth follows: the last one as deep or deeper, so that the game, drawing the
 * list from its end, meets every entry after it, nearer, first (decomp's link_primitive: a polygon goes no nearer than
 * those met before it); NOWHERE if none */
static uint16_t after_by_depth(int32_t depth, int count) {
    int low = 0;
    int high = count - 1;
    int found = -1;

    while (low <= high) {
        int middle = (low + high) / 2;

        if (s_DepthOnward[middle] >= depth) {
            found = middle;
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }
    if (found < 0) {
        return NOWHERE;
    } else {
        return (uint16_t)found;
    }
}

/* An entry beside, unless a list has it: wholly outside the 4:3 picture, placed by its depth among the camera's;
 * reaching into it, before them all, as the camera's list left it out where it would show at 4:3 (hidden there, a
 * face turned away most often, which its depth would put in front of the faces that hide it). One the GTE or the GPU
 * would draw wrong, cut */
static void add_beside(uint16_t entry, int count) {
    Seen seen;
    int outside;

    if (s_Place[entry] != NOWHERE || s_AddedCount >= WIDE_LIST_ENTRIES - SORT_LIST_ENTRIES || !see(entry, &seen)) {
        /* In a list, the list drawn full, or not in the tables */
        return;
    } else if (!seen.cut) {
        if (bounds_beside(seen.low_x, seen.high_x, seen.low_y, seen.high_y, &outside)) {
            s_Added[s_AddedCount] = entry;
            s_AddedAfter[s_AddedCount] = outside ? after_by_depth(seen.depth, count) : NOWHERE;
            s_AddedDepth[s_AddedCount] = seen.depth;
            s_AddedCount++;
            s_Place[entry] = ADDED;
        } else {
            /* Not beside */
        }
    } else {
        int side;

        for (side = PLANE_BESIDE_LEFT; side <= PLANE_BESIDE_RIGHT; side++) {
            CutCorner piece[CUT_CORNERS_MOST * 2];
            int64_t bounds[4];
            int corners = cut_polygon(&seen, entry, side, piece, bounds);

            if (corners > 0 && bounds_beside(bounds[0], bounds[1], bounds[2], bounds[3], &outside)) {
                add_piece(&seen, piece, corners, after_by_depth(seen.depth, count));
            } else {
                /* Nothing left of it on this side */
            }
        }
        s_Place[entry] = ADDED;
    }
}

/* The loaded worlds' polygons beside the 4:3 picture that no list added; of a quad a list draws half of, the other
 * half */
static void add_worlds_beside(int count) {
    int world;

    for (world = 0; world < s_WorldCount; world++) {
        const int32_t *header = world_header(world);
        uint32_t base = (uint32_t)world << ENTRY_WORLD_SHIFT;
        uint32_t index;

        if (header == NULL) {
            continue;
        } else {
            /* Loaded */
        }
        for (index = 0; index < (uint32_t)header[HEADER_TRIANGLES] && index <= ENTRY_INDEX_MASK; index++) {
            add_beside((uint16_t)(base | KIND_TRIANGLE | index), count);
        }
        for (index = 0; index < (uint32_t)header[HEADER_QUADS] && index <= ENTRY_INDEX_MASK; index++) {
            int first_half = s_Place[base | KIND_QUAD_FIRST | index] != NOWHERE;
            int last_half = s_Place[base | KIND_QUAD_LAST | index] != NOWHERE;

            if (first_half && last_half) {
                /* Drawn as its two triangles */
            } else if (first_half) {
                add_beside((uint16_t)(base | KIND_QUAD_LAST | index), count);
            } else if (last_half) {
                add_beside((uint16_t)(base | KIND_QUAD_FIRST | index), count);
            } else {
                add_beside((uint16_t)(base | KIND_QUAD | index), count);
            }
        }
    }
}

uint16_t *Host_WorldSortList(uint16_t *camera) {
    int count = (int16_t)camera[0];
    int index;

    if (!wide() || !s_LargeBuffers || g_CameraPath == 0 || count <= 0 || g_CurrentZone == NULL) {
        return camera;
    } else {
        memset(s_Place, 0xFF, sizeof(s_Place));
    }
    for (index = 0; index < count; index++) {
        s_Place[camera[SORT_LIST_FIRST + index]] = (uint16_t)index;
    }
    s_AddedCount = 0;
    take_scene();
    cut_begin();
    camera_depths(camera, count);
    add_worlds_beside(count);
    cut_end();
    write_wide(camera, count);
    return s_Wide;
}
