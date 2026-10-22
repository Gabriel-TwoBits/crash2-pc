#ifndef PRECISE_H
#define PRECISE_H

#include <stdint.h>

/* The GTE's projections kept before their rounding (precise.c): each screen position the game's vertices were
 * projected to, by its value, with its fraction and its depth, for the GPU renderer to draw the vertex that holds it */

/* decomp's GTE_PROJECTED (include/platform.h) */
void Host_GteProjected(uint32_t sxy, const int64_t *view, uint32_t h, int32_t ofx, int32_t ofy);
/* A game frame's end: the projections since are the next frame's */
void Precise_NewFrame(void);
/* The projection a vertex's position word came from, this frame or the last: its x and y's fractions past the word's
 * own and its depth; 0 if none */
int Precise_Find(uint32_t position, float *dx, float *dy, float *depth);

#endif /* PRECISE_H */
