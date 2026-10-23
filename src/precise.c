#include <stddef.h>
#include <stdint.h>

#include "precise.h"
#include "settings.h"

/* The wide view (the 16:9 shape) narrows each projection across by 3/4, so a 4:3 picture's worth of the world takes
 * three quarters of the screen's width and the rest shows what lay beside it, the picture then shown at 16:9.
 * The GTE rounds a projected vertex to whole pixels (its SXY) and the GPU draws what the game wrote of it, so a vertex
 * moving less than a pixel jumps (the PlayStation's jitter), and a texture is stepped without the depth (its warping).
 * The GTE model's projection (decomp's include/gte.h) hands over the vertex's view coordinates before their rounding:
 * its position is computed again here with their fractions, and kept with its depth by the value the game wrote. The
 * game copies that value from the GTE into its primitives, through its scratchpad or not: the GPU renderer finds it
 * by the value again. Vertices projected to the same pixel in a frame (Crash's before a wall's, 0.7% of them on Turtle
 * Woods) are kept each, up to CANDIDATES: the renderer takes for a polygon's vertex the one whose depth is nearest its
 * other vertices'. TRADEOFF: following each value's way through the game's memory (PGXP) would tell them apart
 * for sure */

#define SLOTS 16384
#define SLOT_MASK (SLOTS - 1)
#define PROBES 8
#define HASH_MULTIPLIER 2654435761u
#define HASH_SHIFT 18
/* The view coordinates' fraction bits, and the projection's offsets' */
#define VIEW_ONE 4096.0
#define OFFSET_ONE 65536.0
#define HALF_MASK 0xFFFF
#define HALF_SHIFT 16
/* The GTE's limits (decomp's include/gte.h): IR1-3, SZ3 and the screen position saturate, and H / SZ3 overflows from
 * H >= 2 * SZ3. Within them the GTE's position is the view's but for its floorings and its division's error (a pixel or
 * two on the screen, more far off it); past one it is another, drawn as the GTE put it */
#define VIEW_SHIFT 12
#define IR_MIN (-0x8000)
#define IR_MAX 0x7FFF
#define SZ_MAX 0xFFFF
#define DIVIDE_LIMIT 2
#define SCREEN_MIN (-0x400)
#define SCREEN_MAX 0x3FF
/* The wide view's narrowing: a 4:3 picture's width in a 16:9 one's */
#define WIDE_NUMERATOR 3
#define WIDE_DENOMINATOR 4

/* Two projections of a pixel within this share of each other's depth are one vertex's, projected again */
#define SAME_DEPTH 0.01

typedef struct {
    uint32_t key;
    uint32_t generation;
    int count;
    Projection projections[CANDIDATES];
} Slot;

static Slot s_Slots[SLOTS];
/* Starts past 1, so an empty slot's 0 is no frame's */
static uint32_t s_Generation = 2;

static int wide(void) {
    return g_Settings[SETTING_SHAPE].value == SHAPE_WIDE;
}

int64_t Host_Horizontal(int64_t offset) {
    if (wide()) {
        return offset * WIDE_NUMERATOR / WIDE_DENOMINATOR;
    } else {
        return offset;
    }
}

/* The same, of a precise offset */
static double narrowed(double offset) {
    if (wide()) {
        return offset * WIDE_NUMERATOR / WIDE_DENOMINATOR;
    } else {
        return offset;
    }
}

static uint32_t hash(uint32_t key) {
    return (key * HASH_MULTIPLIER) >> HASH_SHIFT;
}

static int recent(const Slot *slot) {
    return slot->generation + 1 >= s_Generation;
}

static int within_limits(uint32_t sxy, const int64_t *view, uint32_t h) {
    int64_t ir1 = view[0] >> VIEW_SHIFT;
    int64_t ir2 = view[1] >> VIEW_SHIFT;
    int64_t sz3 = view[2] >> VIEW_SHIFT;
    int16_t sx = (int16_t)(sxy & HALF_MASK);
    int16_t sy = (int16_t)(sxy >> HALF_SHIFT);

    if (ir1 < IR_MIN || ir1 > IR_MAX || ir2 < IR_MIN || ir2 > IR_MAX) {
        return 0;
    } else if (sz3 <= 0 || sz3 > SZ_MAX || (int64_t)h >= DIVIDE_LIMIT * sz3) {
        return 0;
    } else if (sx <= SCREEN_MIN || sx >= SCREEN_MAX || sy <= SCREEN_MIN || sy >= SCREEN_MAX) {
        return 0;
    } else {
        return 1;
    }
}

void Host_GteProjected(uint32_t sxy, const int64_t *view, uint32_t h, int32_t ofx, int32_t ofy) {
    double z = (double)view[2] / VIEW_ONE;
    double x, y, dx, dy;
    uint32_t first = hash(sxy);
    Slot *slot = &s_Slots[first & SLOT_MASK];
    Projection *kept;
    int probe;
    int index;

    if (!within_limits(sxy, view, h)) {
        return;
    } else {
        x = (double)ofx / OFFSET_ONE + narrowed((double)h * ((double)view[0] / VIEW_ONE) / z);
        y = (double)ofy / OFFSET_ONE + (double)h * ((double)view[1] / VIEW_ONE) / z;
        dx = x - (double)(int16_t)(sxy & HALF_MASK);
        dy = y - (double)(int16_t)(sxy >> HALF_SHIFT);
    }
    /* Kept: its own slot, else a free or old one along the probes, else the last frame's first (drawn by now), else the
     * first */
    for (probe = 0; probe < PROBES; probe++) {
        Slot *candidate = &s_Slots[(first + (uint32_t)probe) & SLOT_MASK];

        if (candidate->key == sxy || !recent(candidate)) {
            slot = candidate;
            break;
        } else {
            continue;
        }
    }
    if (probe == PROBES) {
        for (probe = 0; probe < PROBES; probe++) {
            Slot *candidate = &s_Slots[(first + (uint32_t)probe) & SLOT_MASK];

            if (candidate->generation != s_Generation) {
                slot = candidate;
                break;
            } else {
                continue;
            }
        }
    } else {
        /* Found along the probes */
    }
    if (slot->key != sxy || slot->generation != s_Generation) {
        slot->key = sxy;
        slot->generation = s_Generation;
        slot->count = 0;
    } else {
        /* This frame's pixel again */
    }
    kept = NULL;
    for (index = 0; index < slot->count; index++) {
        double apart = (double)slot->projections[index].depth - z;

        if (apart < z * SAME_DEPTH && apart > -z * SAME_DEPTH) {
            kept = &slot->projections[index];
            break;
        } else {
            continue;
        }
    }
    if (kept != NULL) {
        /* The same vertex's */
    } else if (slot->count < CANDIDATES) {
        kept = &slot->projections[slot->count++];
    } else {
        kept = &slot->projections[CANDIDATES - 1];
    }
    kept->dx = (float)dx;
    kept->dy = (float)dy;
    kept->depth = (float)z;
}

void Precise_NewFrame(void) {
    s_Generation++;
}

int Precise_Find(uint32_t position, Projection *found) {
    uint32_t first = hash(position);
    int probe;
    int index;

    for (probe = 0; probe < PROBES; probe++) {
        const Slot *slot = &s_Slots[(first + (uint32_t)probe) & SLOT_MASK];

        if (slot->key == position && recent(slot)) {
            for (index = 0; index < slot->count; index++) {
                found[index] = slot->projections[index];
            }
            return slot->count;
        } else {
            continue;
        }
    }
    return 0;
}
