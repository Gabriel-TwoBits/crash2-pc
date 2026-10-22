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
 * A polygon reaching behind the camera is left out, a hole beside it where it would show: clipped at the camera, its
 * pieces drawn by the host, it would show */

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

/* A world's vertex as the camera sees it: its position from the camera (the GTE's VXY pair and VZ, as Gfx_DrawWorlds
 * loads it) turned by the GTE's rotation */
static void view_vertex(const uint32_t *world, uint32_t index, int64_t *view) {
    const unsigned char *vertices = (const unsigned char *)(uintptr_t)world[BLOCK_VERTICES];
    uint32_t word = *(const uint32_t *)(vertices - (index + 1) * WORD_BYTES);
    uint32_t half = *(const uint16_t *)(vertices + index * sizeof(uint16_t));
    uint32_t pair = ((word & VERTEX_MASK) + world[BLOCK_OFFSET_XY]) & PAIR_MASK;
    int64_t position[AXES];
    int axis;

    position[0] = (int16_t)pair;
    position[1] = (int16_t)(pair >> HALF_SHIFT);
    position[2] = (int16_t)((half & VERTEX_Z_MASK) + world[BLOCK_OFFSET_Z]);
    for (axis = 0; axis < AXES; axis++) {
        view[axis] = (s_Rotation[axis][0] * position[0] + s_Rotation[axis][1] * position[1] +
                      s_Rotation[axis][2] * position[2]) >> GTE_FRACTION_SHIFT;
    }
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

/* Whether an entry's polygon shows beside the 4:3 picture: every corner in front of the camera (one behind it the GTE
 * projects outward, stretching the polygon over what lies beside: tried, holes and smears at Turtle Woods' edges), its
 * bounds as the GTE projects it on the screen and past the 4:3 picture's edge on one side, both with the margin, and
 * the GPU drawing it. Its depth, the sum of those of the corners the game's depth slot sums, and whether it lies
 * wholly outside the 4:3 picture */
static int beside(uint16_t entry, int32_t *depth, int *outside) {
    int world = entry >> ENTRY_WORLD_SHIFT;
    const int32_t *header = world_header(world);
    const uint32_t *block = world_block(world);
    uint32_t corners[QUAD_CORNERS];
    int count = header == NULL ? 0 : entry_corners(entry, header, corners);
    int64_t h = g_HostGte[GTE_CONTROL + GTE_H] & GTE_H_MASK;
    int64_t half_width = (int64_t)(g_HostGte[GTE_CONTROL + GTE_OFX] >> HALF_SHIFT);
    int64_t half_height = (int64_t)(g_HostGte[GTE_CONTROL + GTE_OFY] >> HALF_SHIFT);
    int64_t band = half_width * WIDE_NUMERATOR / WIDE_DENOMINATOR;
    int64_t low_x = INT64_MAX, high_x = INT64_MIN, low_y = INT64_MAX, high_y = INT64_MIN;
    int behind = 0;
    int corner;

    *depth = 0;
    *outside = 0;
    if (count == 0) {
        return 0;
    } else {
        /* Its corners in their tables */
    }
    for (corner = 0; corner < count; corner++) {
        int64_t view[AXES];
        int64_t x, y;

        if (corners[corner] >= (uint32_t)header[HEADER_VERTICES]) {
            return 0;
        } else {
            view_vertex(block, corners[corner], view);
        }
        if (corner >= count - TRIANGLE_CORNERS) {
            *depth += (int32_t)view[2];
        } else {
            /* A quad's first corner, which the game's depth slot leaves out */
        }
        if (view[2] * QUOTIENT_MOST <= h) {
            x = clamped(view[0], GTE_IR_MIN, GTE_IR_MAX) * QUOTIENT_MOST * WIDE_NUMERATOR / WIDE_DENOMINATOR;
            y = clamped(view[1], GTE_IR_MIN, GTE_IR_MAX) * QUOTIENT_MOST;
        } else {
            x = h * view[0] / view[2] * WIDE_NUMERATOR / WIDE_DENOMINATOR;
            y = h * view[1] / view[2];
        }
        behind |= view[2] <= 0;
        x = clamped(x, GTE_SCREEN_MIN - half_width, GTE_SCREEN_MAX - half_width);
        y = clamped(y, GTE_SCREEN_MIN - half_height, GTE_SCREEN_MAX - half_height);
        low_x = x < low_x ? x : low_x;
        high_x = x > high_x ? x : high_x;
        low_y = y < low_y ? y : low_y;
        high_y = y > high_y ? y : high_y;
    }
    *outside = low_x > band || high_x < -band;
    return !behind && high_x - low_x < GPU_POLYGON_WIDTH && high_y - low_y < GPU_POLYGON_HEIGHT &&
           high_x > -half_width - BESIDE_MARGIN && low_x < half_width + BESIDE_MARGIN &&
           high_y > -half_height - BESIDE_MARGIN && low_y < half_height + BESIDE_MARGIN &&
           (high_x > band - BESIDE_MARGIN || low_x < BESIDE_MARGIN - band);
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

/* The camera's entries' depths, and the deepest of each from it on */
static void camera_depths(const uint16_t *camera, int count) {
    int index;

    for (index = 0; index < count; index++) {
        int32_t depth;
        int outside;

        beside(camera[SORT_LIST_FIRST + index], &depth, &outside);
        s_CameraDepth[index] = depth;
    }
    for (index = count - 1; index >= 0; index--) {
        if (index == count - 1 || s_CameraDepth[index] > s_DepthOnward[index + 1]) {
            s_DepthOnward[index] = s_CameraDepth[index];
        } else {
            s_DepthOnward[index] = s_DepthOnward[index + 1];
        }
    }
}

/* An entry beside, unless a list has it: wholly outside the 4:3 picture, placed by its depth among the camera's;
 * reaching into it, before them all, as the camera's list left it out where it would show at 4:3 (hidden there, a
 * face turned away most often, which its depth would put in front of the faces that hide it) */
static void add_beside(uint16_t entry, int count) {
    int32_t depth;
    int outside;

    if (s_Place[entry] != NOWHERE || s_AddedCount >= WIDE_LIST_ENTRIES - SORT_LIST_ENTRIES) {
        /* In a list, or the list drawn full */
        return;
    } else if (beside(entry, &depth, &outside)) {
        s_Added[s_AddedCount] = entry;
        s_AddedAfter[s_AddedCount] = outside ? after_by_depth(depth, count) : NOWHERE;
        s_AddedDepth[s_AddedCount] = depth;
        s_AddedCount++;
        s_Place[entry] = ADDED;
    } else {
        /* Not beside */
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
    camera_depths(camera, count);
    add_worlds_beside(count);
    write_wide(camera, count);
    return s_Wide;
}
