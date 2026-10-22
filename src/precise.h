#ifndef PRECISE_H
#define PRECISE_H

#include <stdint.h>

/* The GTE's projections kept before their rounding (precise.c): each screen position the game's vertices were
 * projected to, by its value, with its fraction and its depth, for the GPU renderer to draw the vertex that holds it */

/* decomp's GTE_HORIZONTAL (include/platform.h): the offset narrowed by the wide view's 3/4 (4:3 in 16:9) */
int64_t Host_Horizontal(int64_t offset);
/* decomp's GTE_PROJECTED */
void Host_GteProjected(uint32_t sxy, const int64_t *view, uint32_t h, int32_t ofx, int32_t ofy);
/* A game frame's end: the projections since are the next frame's */
void Precise_NewFrame(void);
/* A projection: its x and y's fractions past its position word's own, and its depth */
typedef struct {
    float dx, dy;
    float depth;
} Projection;

/* The projections a vertex's position word may have come from, this frame or the last, into found (CANDIDATES at
 * most): their count, 0 if none */
#define CANDIDATES 4
int Precise_Find(uint32_t position, Projection *found);

#endif /* PRECISE_H */
