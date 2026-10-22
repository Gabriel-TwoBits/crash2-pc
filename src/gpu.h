#ifndef GPU_H
#define GPU_H

#include <stdint.h>

/* The PlayStation's GPU in software (gpu.c): its 1 MB of VRAM, 1024 by 512 pixels of 16 bits, and the GP0 commands
 * that draw into it */

#define VRAM_WIDTH 1024
#define VRAM_HEIGHT 512

extern uint16_t g_Vram[VRAM_HEIGHT][VRAM_WIDTH];

/* One word to GP0, as the CPU or the DMA writes it */
void Gpu_Write(uint32_t word);
/* VRAM's rectangle to and from memory, a pixel a halfword (LoadImage, StoreImage) */
void Gpu_LoadImage(int x, int y, int width, int height, const uint16_t *pixels);
void Gpu_StoreImage(int x, int y, int width, int height, uint16_t *pixels);
void Gpu_MoveImage(int x, int y, int width, int height, int to_x, int to_y);
/* The drawing environment's state as libgpu sets it: the drawing area (inclusive), the offset */
void Gpu_SetDrawArea(int left, int top, int right, int bottom);
void Gpu_SetDrawOffset(int x, int y);

#endif /* GPU_H */
