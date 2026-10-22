#include <stdint.h>
#include <string.h>

#include "host_log.h"
#include "settings.h"
#include "world_view.h"

/* The world's polygons beside the camera's view, for the wide view (the 16:9 shape). The game draws only the polygons
 * its camera path's sort list holds at the camera's node (an SLST entry: the list at the path's first node, then each
 * node's delta from the one before), made for a 4:3 view: wider, the edges showed the background where the world
 * stopped. So the list drawn is the camera's own with polygons added:
 * - on the side-on paths, those of the lists NODES_AROUND nodes ahead and behind, each placed after the polygon it
 *   follows in its own list (the drawing goes by its depth slots, the order within one alone), of the worlds the
 *   camera's list draws from (another's may not be loaded), decoded from the camera's by the game's own decoder;
 * - on every path, the loaded worlds' polygons no list added that show beside the 4:3 picture, found by projecting
 *   their corners as the GTE will, placed before the camera's.
 * More polygons take more primitives than the game's frame buffers hold (76 of their 89 KB used on Turtle Woods, 102
 * with the polygons added): with the wide view chosen, its frame buffers are the host's own, larger, from the level's
 * opening or from the end of the frame it was chosen in. The objects come and go as the game's draw lists have them:
 * one at the edge may still come in late.
 * TRADEOFF: a fixed count of nodes around; how far a node reaches across differs from path to path. The polygons
 * beside go first within a depth slot, in no order of the game's lists; their order would take a sort by depth */

#define NODES_AROUND 12
/* The camera paths that look at the world side-on, whose nodes ahead and behind lie across the view (path kinds 3 and
 * 8, by Crash2Recomp's notes: decomp's config/symbol_addrs.txt). Along a path into the view they lie ahead of the
 * camera and behind it and add nothing seen (Turtle Woods' pictures the same to the byte): those keep the camera's
 * list */
#define PATH_SIDE_ON 3
#define PATH_SIDE_ON_TOO 8
/* The sort lists: a count, a word unused, then the entries (decomp's 80039D2C.c, SORT_LIST_ENTRIES) */
#define SORT_LIST_ENTRIES 0x5F0
#define SORT_LIST_FIRST 2
#define SORT_LIST_HALVES (SORT_LIST_FIRST + SORT_LIST_ENTRIES)
/* The list drawn: the camera's and the others' added, three lists' worth at most */
#define WIDE_LIST_ENTRIES (SORT_LIST_ENTRIES * 3)
#define ENTRY_VALUES 65536
/* An entry's world, of the zone's 8 (decomp's 80039D2C.c, ENTRY_WORLD_SHIFT) */
#define ENTRY_WORLD_SHIFT 13
#define NOWHERE 0xFFFF
#define ADDED 0xFFFE
/* The entity's property for its sort list (SLST), as Cam_MoveAlongPath reads it, and an entry's items from 0x10 */
#define PROPERTY_SORT_LIST 0x103
#define PROPERTY_ROW_KIND 2
#define ENTRY_ITEMS 0x10
/* A word's low bit: an EID (or a page's), not a pointer */
#define EID_TAG 1
#define WORD_BYTES 4
#define FORWARD 1
#define BACKWARD 0
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
/* The GTE divides H by a depth up to H / 2 at most (its division's limit): a corner nearer is not drawn right */
#define DIVISION_LIMIT_SHARE 2
#define AXES 3
/* The wide view's narrowing (precise.c's): the 4:3 picture's half width a share of the screen's */
#define WIDE_NUMERATOR 3
#define WIDE_DENOMINATOR 4
extern unsigned char g_HostScratchpad[];
extern unsigned char *g_CurrentZone;

/* The game's (decomp's include/game.h, 80026CA0.c) */
int32_t Heap_Alloc(int32_t bytes);
void Heap_Free(int32_t buffer);
int32_t Ent_GetProperty(int32_t entity, int32_t property, int32_t row, int32_t column, int32_t *out, int32_t kind,
                        int32_t *count, int32_t a, int32_t b);
uint16_t *func_80029800(uint16_t *delta, int32_t from, uint16_t *to, int32_t forward);
extern int32_t g_CameraPath;
extern int32_t g_CameraNode;
extern int32_t g_CameraNodeCount;
extern uint32_t g_CameraPathKind;

static unsigned char s_Primitives[HOST_FRAME_BUFFERS][HOST_PRIMITIVE_BYTES] __attribute__((aligned(16)));
static int s_PrimitivesTaken[HOST_FRAME_BUFFERS];
/* Whether the frame buffers in use are the host's (the wide view chosen when the level opened, or since) */
static int s_LargeBuffers;

static uint16_t s_Around[2][SORT_LIST_HALVES];
static uint16_t s_Wide[SORT_LIST_FIRST + WIDE_LIST_ENTRIES];
/* An entry's place in the camera's list (NOWHERE if not there, ADDED if added already), and the entries added with
 * the camera's entry each follows */
static uint16_t s_Place[ENTRY_VALUES];
static uint16_t s_Added[WIDE_LIST_ENTRIES];
static uint16_t s_AddedAfter[WIDE_LIST_ENTRIES];
static int s_AddedCount;
/* The worlds the camera's list draws from, a bit each */
static uint32_t s_WorldsUsed;
static int s_SlotCount[SORT_LIST_ENTRIES + 1];
static int s_SlotStart[SORT_LIST_ENTRIES + 1];
static int s_SlotFill[SORT_LIST_ENTRIES + 1];
static uint16_t s_Sorted[WIDE_LIST_ENTRIES];

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

/* The list at a node near the camera's: the camera's decoded steps nodes along, or as far as the path goes. The
 * decoding goes from a copy: backward, it undoes the delta's swaps on the list it starts from, which would leave the
 * game's own list scrambled for its next decoding */
static const uint16_t *list_at(const uint16_t *camera, uint16_t *const *items, int steps, int direction,
                               uint16_t (*buffers)[SORT_LIST_HALVES]) {
    int current = 0;
    int step;

    memcpy(buffers[current], camera, sizeof(camera[0]) * (size_t)(SORT_LIST_FIRST + (int16_t)camera[0]));
    for (step = 0; step < steps; step++) {
        int node = g_CameraNode + (direction == FORWARD ? step + 1 : -step);

        if (node < 1 || node >= g_CameraNodeCount) {
            break;
        } else {
            func_80029800(items[node], (int32_t)(uintptr_t)buffers[current], buffers[!current], direction);
            current = !current;
        }
    }
    return buffers[current];
}

/* Another list's entries the camera's lacks, each after the camera's entry it follows there (or before them all) */
static void add_from(const uint16_t *list) {
    uint16_t after = NOWHERE;
    int index;

    for (index = 0; index < (int16_t)list[0]; index++) {
        uint16_t entry = list[SORT_LIST_FIRST + index];

        if (s_Place[entry] == ADDED) {
            /* Added from a list before */
        } else if (s_Place[entry] != NOWHERE) {
            after = s_Place[entry];
        } else if (!(s_WorldsUsed & (1u << (entry >> ENTRY_WORLD_SHIFT)))) {
            /* A world the camera's list doesn't draw from, which may not be loaded */
        } else if (s_AddedCount < WIDE_LIST_ENTRIES - SORT_LIST_ENTRIES) {
            s_Added[s_AddedCount] = entry;
            s_AddedAfter[s_AddedCount] = after;
            s_AddedCount++;
            s_Place[entry] = ADDED;
        } else {
            /* The list drawn full */
        }
    }
}

/* The list drawn: the entries added before the camera's first, then each of the camera's followed by those added
 * after it, sorted into their slots by a count of each slot's */
static void write_wide(const uint16_t *camera, int count) {
    int slots = count + 1;
    int written = 0;
    int start = 0;
    int index;

    memset(s_SlotCount, 0, sizeof(s_SlotCount[0]) * (size_t)slots);
    for (index = 0; index < s_AddedCount; index++) {
        s_SlotCount[slot_after(s_AddedAfter[index])]++;
    }
    for (index = 0; index < slots; index++) {
        s_SlotStart[index] = start;
        s_SlotFill[index] = start;
        start += s_SlotCount[index];
    }
    for (index = 0; index < s_AddedCount; index++) {
        s_Sorted[s_SlotFill[slot_after(s_AddedAfter[index])]++] = s_Added[index];
    }
    for (index = 0; index < slots; index++) {
        int added;

        if (index > 0) {
            s_Wide[SORT_LIST_FIRST + written++] = camera[SORT_LIST_FIRST + index - 1];
        } else {
            /* The slot before the camera's entries */
        }
        for (added = s_SlotStart[index]; added < s_SlotStart[index] + s_SlotCount[index]; added++) {
            s_Wide[SORT_LIST_FIRST + written++] = s_Sorted[added];
        }
    }
    s_Wide[0] = (uint16_t)written;
    s_Wide[1] = camera[1];
}

/* A world's vertex as the camera sees it: its position from the camera (the GTE's VXY pair and VZ, as Gfx_DrawWorlds
 * loads it) turned by the GTE's rotation */
static void view_vertex(const uint32_t *world, uint32_t index, const int32_t rotation[AXES][AXES], int64_t *view) {
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
        view[axis] = (rotation[axis][0] * position[0] + rotation[axis][1] * position[1] +
                      rotation[axis][2] * position[2]) >> GTE_FRACTION_SHIFT;
    }
}

static void gte_rotation(int32_t rotation[AXES][AXES]) {
    const unsigned int *control = &g_HostGte[GTE_CONTROL];

    rotation[0][0] = (int16_t)control[GTE_RT11RT12];
    rotation[0][1] = (int16_t)(control[GTE_RT11RT12] >> HALF_SHIFT);
    rotation[0][2] = (int16_t)control[GTE_RT13RT21];
    rotation[1][0] = (int16_t)(control[GTE_RT13RT21] >> HALF_SHIFT);
    rotation[1][1] = (int16_t)control[GTE_RT22RT23];
    rotation[1][2] = (int16_t)(control[GTE_RT22RT23] >> HALF_SHIFT);
    rotation[2][0] = (int16_t)control[GTE_RT31RT32];
    rotation[2][1] = (int16_t)(control[GTE_RT31RT32] >> HALF_SHIFT);
    rotation[2][2] = (int16_t)control[GTE_RT33];
}

/* Whether a polygon shows beside the 4:3 picture: every corner in front of the camera, past the GTE's division's
 * limit, and its projected bounds on the screen, past the 4:3 picture's edge on one side */
static int beside(const uint32_t *world, const uint32_t *corners, int count, const int32_t rotation[AXES][AXES],
                  uint32_t vertex_count) {
    int64_t h = g_HostGte[GTE_CONTROL + GTE_H] & GTE_H_MASK;
    int64_t half_width = (int64_t)(g_HostGte[GTE_CONTROL + GTE_OFX] >> HALF_SHIFT);
    int64_t half_height = (int64_t)(g_HostGte[GTE_CONTROL + GTE_OFY] >> HALF_SHIFT);
    int64_t band = half_width * WIDE_NUMERATOR / WIDE_DENOMINATOR;
    int64_t low_x = INT64_MAX, high_x = INT64_MIN, low_y = INT64_MAX, high_y = INT64_MIN;
    int corner;

    for (corner = 0; corner < count; corner++) {
        int64_t view[AXES];
        int64_t x, y;

        if (corners[corner] >= vertex_count) {
            return 0;
        } else {
            view_vertex(world, corners[corner], rotation, view);
        }
        if (view[2] * DIVISION_LIMIT_SHARE <= h) {
            return 0;
        } else {
            x = h * view[0] / view[2] * WIDE_NUMERATOR / WIDE_DENOMINATOR;
            y = h * view[1] / view[2];
        }
        low_x = x < low_x ? x : low_x;
        high_x = x > high_x ? x : high_x;
        low_y = y < low_y ? y : low_y;
        high_y = y > high_y ? y : high_y;
    }
    return high_x > -half_width && low_x < half_width && high_y > -half_height && low_y < half_height &&
           (high_x > band || low_x < -band);
}

/* A polygon beside, before the camera's entries, unless a list has it */
static void add_beside(uint16_t entry) {
    if (s_Place[entry] != NOWHERE) {
        return;
    } else if (s_AddedCount < WIDE_LIST_ENTRIES - SORT_LIST_ENTRIES) {
        s_Added[s_AddedCount] = entry;
        s_AddedAfter[s_AddedCount] = NOWHERE;
        s_AddedCount++;
        s_Place[entry] = ADDED;
    } else {
        /* The list drawn full */
    }
}

/* The loaded worlds' polygons beside the 4:3 picture that no list added, before all the camera's */
static void add_worlds_beside(void) {
    const int32_t *zone = *(const int32_t *const *)(g_CurrentZone + ZONE_ITEMS);
    const uint32_t *worlds = (const uint32_t *)(g_HostScratchpad + SCRATCH_WORLDS);
    int32_t rotation[AXES][AXES];
    int count = zone[0] < WORLDS ? zone[0] : WORLDS;
    int world;

    gte_rotation(rotation);
    for (world = 0; world < count; world++) {
        const int32_t *header = (const int32_t *)(uintptr_t)zone[ZONE_WORLD_FIRST / WORD_BYTES +
                                                               ZONE_WORLD_WORDS * world + ZONE_WORLD_HEADER];
        const uint32_t *block = worlds + world * WORLD_BLOCK_WORDS;
        uint32_t vertex_count;
        uint32_t index;
        uint32_t base = (uint32_t)world << ENTRY_WORLD_SHIFT;

        if (header == NULL || ((uintptr_t)header & EID_TAG)) {
            /* Not loaded */
            continue;
        } else {
            vertex_count = (uint32_t)header[HEADER_VERTICES];
        }
        for (index = 0; index < (uint32_t)header[HEADER_TRIANGLES] && index <= ENTRY_INDEX_MASK; index++) {
            const unsigned char *triangles = (const unsigned char *)(uintptr_t)block[BLOCK_TRIANGLES];
            uint32_t first = *(const uint32_t *)(triangles - (index + 1) * WORD_BYTES);
            uint32_t second = *(const uint16_t *)(triangles + index * sizeof(uint16_t));
            uint32_t corners[TRIANGLE_CORNERS];

            corners[0] = first >> VERTEX_HIGH_SHIFT;
            corners[1] = (first >> VERTEX_LOW_SHIFT) & VERTEX_INDEX_MASK;
            corners[2] = (second >> VERTEX_HALF_SHIFT) & VERTEX_INDEX_MASK;
            if (beside(block, corners, TRIANGLE_CORNERS, rotation, vertex_count)) {
                add_beside((uint16_t)(base | KIND_TRIANGLE | index));
            } else {
                continue;
            }
        }
        for (index = 0; index < (uint32_t)header[HEADER_QUADS] && index <= ENTRY_INDEX_MASK; index++) {
            const uint32_t *quads = (const uint32_t *)(uintptr_t)block[BLOCK_QUADS];
            uint32_t first = quads[index * QUAD_RECORD_WORDS];
            uint32_t second = quads[index * QUAD_RECORD_WORDS + 1];
            uint32_t corners[QUAD_CORNERS];

            if (s_Place[base | KIND_QUAD_FIRST | index] != NOWHERE ||
                s_Place[base | KIND_QUAD_LAST | index] != NOWHERE) {
                /* Drawn as its two triangles */
                continue;
            } else {
                corners[0] = first >> VERTEX_HIGH_SHIFT;
                corners[1] = (first >> VERTEX_LOW_SHIFT) & VERTEX_INDEX_MASK;
                corners[2] = second >> VERTEX_HIGH_SHIFT;
                corners[3] = (second >> VERTEX_LOW_SHIFT) & VERTEX_INDEX_MASK;
            }
            if (beside(block, corners, QUAD_CORNERS, rotation, vertex_count)) {
                add_beside((uint16_t)(base | KIND_QUAD | index));
            } else {
                continue;
            }
        }
    }
}

/* An entry by its reference if the paging has it in memory, NULL if not: as Nsf_OpenEntry finds it, without its
 * opening (which pages it in and counts a holder) */
static unsigned char *resident(const uint32_t *reference) {
    uint32_t record;
    uint32_t entry;

    if (reference == NULL) {
        return NULL;
    } else {
        record = *reference;
    }
    if ((record & EID_TAG) || record == 0) {
        /* An EID not looked up yet */
        return NULL;
    } else {
        entry = *(const uint32_t *)(uintptr_t)record;
    }
    if (entry & EID_TAG) {
        /* Its page not in memory */
        return NULL;
    } else {
        return (unsigned char *)(uintptr_t)entry;
    }
}

uint16_t *Host_WorldSortList(uint16_t *camera) {
    const uint32_t *reference;
    unsigned char *entry = NULL;
    int count = (int16_t)camera[0];
    int index;

    if (!wide() || !s_LargeBuffers || g_CameraPath == 0 || count <= 0 || g_CurrentZone == NULL) {
        return camera;
    } else if (g_CameraPathKind == PATH_SIDE_ON || g_CameraPathKind == PATH_SIDE_ON_TOO) {
        reference = (const uint32_t *)(uintptr_t)Ent_GetProperty(g_CameraPath, PROPERTY_SORT_LIST, 0, 0, 0,
                                                                 PROPERTY_ROW_KIND, 0, 0, 0);
        entry = resident(reference);
    } else {
        /* Along a path into the view, the worlds beside alone */
    }
    memset(s_Place, 0xFF, sizeof(s_Place));
    s_WorldsUsed = 0;
    for (index = 0; index < count; index++) {
        s_Place[camera[SORT_LIST_FIRST + index]] = (uint16_t)index;
        s_WorldsUsed |= 1u << (camera[SORT_LIST_FIRST + index] >> ENTRY_WORLD_SHIFT);
    }
    s_AddedCount = 0;
    if (entry != NULL) {
        uint16_t *const *items = (uint16_t *const *)(entry + ENTRY_ITEMS);

        add_from(list_at(camera, items, NODES_AROUND, FORWARD, s_Around));
        add_from(list_at(camera, items, NODES_AROUND, BACKWARD, s_Around));
    } else {
        /* No lists around */
    }
    add_worlds_beside();
    write_wide(camera, count);
    return s_Wide;
}
