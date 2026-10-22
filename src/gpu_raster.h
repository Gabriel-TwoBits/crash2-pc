#ifndef GPU_RASTER_H
#define GPU_RASTER_H

#include <stdint.h>

/* The GPU's rasterizer: SwanStation's software backend (third_party/swanstation), behind a C interface for gpu.c,
 * which parses the GP0 commands and hands each primitive over as SwanStation's front end does */

#ifdef __cplusplus
extern "C" {
#endif

/* A vertex: its position (the drawing offset added), colour and texture coordinates, and its position's word as the
 * game wrote it, by which the GPU renderer finds its projection (precise.h) */
typedef struct {
    int32_t x, y;
    uint32_t color;
    uint16_t texcoord;
    uint32_t position;
} RasterVertex;

/* The state a primitive draws with: its command word (the first, colour included), the draw mode (E1's bits, a
 * textured polygon's page among them), its CLUT, the texture window (E2's value) and the mask bits (E6's value) */
typedef struct {
    uint32_t command;
    uint16_t draw_mode;
    uint16_t palette;
    uint32_t texture_window;
    uint32_t mask;
} RasterState;

void Raster_Init(uint16_t *vram);
void Raster_SetDrawingArea(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);
void Raster_Polygon(const RasterState *state, int count, const RasterVertex *vertices);
void Raster_Line(const RasterState *state, int count, const RasterVertex *vertices);
void Raster_Rectangle(const RasterState *state, int32_t x, int32_t y, uint16_t width, uint16_t height,
                      uint16_t texcoord);
void Raster_Fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
void Raster_Copy(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t width, uint32_t height,
                 uint32_t mask);
void Raster_Load(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint16_t *pixels, uint32_t mask);

#ifdef __cplusplus
}
#endif

#endif /* GPU_RASTER_H */
