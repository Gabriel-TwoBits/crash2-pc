#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

/* The sound's output (audio.c): opened with the window, fed each frame */
void Host_AudioStart(void);
void Host_AudioFrame(uint32_t cycles);

#endif /* AUDIO_H */
