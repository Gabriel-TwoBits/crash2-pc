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
 * by the value again. TRADEOFF: two vertices projected to the same pixel in a frame keep the later's fraction and
 * depth; tracking each value's way through the game's memory (PGXP) would tell them apart */

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
/* A position further than this from the GTE's own is one it saturated or divided otherwise: not kept */
#define FRACTION_TOLERANCE 1.5
/* The wide view's narrowing: a 4:3 picture's width in a 16:9 one's */
#define WIDE_NUMERATOR 3
#define WIDE_DENOMINATOR 4

typedef struct {
    uint32_t key;
    uint32_t generation;
    float dx, dy;
    float depth;
} Projection;

static Projection s_Slots[SLOTS];
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

static int recent(const Projection *slot) {
    return slot->generation + 1 >= s_Generation;
}

void Host_GteProjected(uint32_t sxy, const int64_t *view, uint32_t h, int32_t ofx, int32_t ofy) {
    double z = (double)view[2] / VIEW_ONE;
    double x, y, dx, dy;
    uint32_t first = hash(sxy);
    Projection *slot = &s_Slots[first & SLOT_MASK];
    int probe;

    if (z <= 0.0) {
        return;
    } else {
        x = (double)ofx / OFFSET_ONE + narrowed((double)h * ((double)view[0] / VIEW_ONE) / z);
        y = (double)ofy / OFFSET_ONE + (double)h * ((double)view[1] / VIEW_ONE) / z;
        dx = x - (double)(int16_t)(sxy & HALF_MASK);
        dy = y - (double)(int16_t)(sxy >> HALF_SHIFT);
    }
    if (dx < -FRACTION_TOLERANCE || dx > FRACTION_TOLERANCE || dy < -FRACTION_TOLERANCE || dy > FRACTION_TOLERANCE) {
        return;
    } else {
        /* Kept: its own slot, else a free or old one along the probes, else the first */
    }
    for (probe = 0; probe < PROBES; probe++) {
        Projection *candidate = &s_Slots[(first + (uint32_t)probe) & SLOT_MASK];

        if (candidate->key == sxy || !recent(candidate)) {
            slot = candidate;
            break;
        } else {
            continue;
        }
    }
    slot->key = sxy;
    slot->generation = s_Generation;
    slot->dx = (float)dx;
    slot->dy = (float)dy;
    slot->depth = (float)z;
}

void Precise_NewFrame(void) {
    s_Generation++;
}

int Precise_Find(uint32_t position, float *dx, float *dy, float *depth) {
    uint32_t first = hash(position);
    int probe;

    for (probe = 0; probe < PROBES; probe++) {
        const Projection *slot = &s_Slots[(first + (uint32_t)probe) & SLOT_MASK];

        if (slot->key == position && recent(slot)) {
            *dx = slot->dx;
            *dy = slot->dy;
            *depth = slot->depth;
            return 1;
        } else {
            continue;
        }
    }
    return 0;
}
