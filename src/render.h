#ifndef RENDER_H
#define RENDER_H

#include <SDL3/SDL.h>
#include <stdint.h>

#include "gpu_raster.h"

/* The GPU renderer (render.c): SDL_GPU on Vulkan draws the GPU's primitives again at a multiple of the PlayStation's
 * resolution, in true colour, beside the software rasterizer, whose VRAM stays the game's (and the textures' source).
 * The window shows its picture, or the software one at the PlayStation's resolution */

/* The device, for the window (or none, headless): 0 when SDL_GPU has none to give, the window then the
 * SDL_Renderer's */
int Render_Start(SDL_Window *window);
/* Whether it runs, and draws at a multiple of the resolution (the settings' resolution above 1) */
int Render_Active(void);
int Render_Drawing(void);
/* The settings' resolution, the presentation's shape and smoothing (settings.h), applied */
void Render_ApplySettings(void);

/* The GPU's primitives and VRAM operations, as gpu.c hands them to the software rasterizer; a load and a copy after
 * the rasterizer's own */
void Render_SetDrawingArea(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);
void Render_Polygon(const RasterState *state, int count, const RasterVertex *vertices);
void Render_Line(const RasterState *state, int count, const RasterVertex *vertices);
void Render_Rectangle(const RasterState *state, int32_t x, int32_t y, uint16_t width, uint16_t height,
                      uint16_t texcoord);
void Render_Fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
void Render_Copy(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t width, uint32_t height);
void Render_Load(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

/* A vertical blank's picture: VRAM's displayed rectangle, from the renderer's target, or the software picture's
 * pixels (0xRRGGBB, width by height) at the PlayStation's resolution; shown in the window, if any */
void Render_Present(int x, int y, int width, int height, int enabled, const uint32_t *pixels);
/* The last picture's displayed rectangle at the renderer's resolution as a PPM (a test's) */
void Render_Shot(const char *path);

#endif /* RENDER_H */
