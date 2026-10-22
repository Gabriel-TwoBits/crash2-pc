#ifndef SPU_HOST_H
#define SPU_HOST_H

#include <stdint.h>

/* The SPU: SwanStation's (third_party/swanstation), behind a C interface for libspu (psyq/libspu.c) and the audio
 * output (audio.c). Its registers by their address (0x1F801C00 on), its 512 KB of RAM, and the frames it generates
 * for the CPU cycles that pass */

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_CHANNELS 2

void Spu_Init(void);
void Spu_WriteRegister(uint32_t address, uint16_t value);
uint16_t Spu_ReadRegister(uint32_t address);
void Spu_WriteRam(uint32_t address, const void *data, uint32_t bytes);
/* Runs the SPU for the CPU cycles given; the stereo frames it generated are then Spu_Frames' Spu_FrameCount, until
 * Spu_ClearFrames */
void Spu_Run(uint32_t cycles);
const int16_t *Spu_Frames(void);
uint32_t Spu_FrameCount(void);
void Spu_ClearFrames(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_HOST_H */
