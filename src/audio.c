#include <SDL3/SDL.h>

#include "audio.h"
#include "host_log.h"
#include "spu_host.h"

/* The SPU's output: SwanStation's SPU run for each frame's CPU cycles (spu_host.h), its 44.1 kHz stereo frames to an
 * SDL3 audio stream when a window opened one. Headless, the SPU still runs, its state being the game's, and its
 * frames are dropped */

#define SAMPLE_RATE 44100
/* TRADEOFF: past a quarter second queued, the frames are dropped, keeping the sound with the picture */
#define MAX_QUEUED_BYTES (SAMPLE_RATE / 4 * SPU_CHANNELS * (int)sizeof(int16_t))

static SDL_AudioStream *s_Stream;

void Host_AudioStart(void) {
    SDL_AudioSpec spec;

    spec.format = SDL_AUDIO_S16;
    spec.channels = SPU_CHANNELS;
    spec.freq = SAMPLE_RATE;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        Host_Log("no sound (%s)", SDL_GetError());
        return;
    } else {
        s_Stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    }
    if (s_Stream == NULL) {
        Host_Log("no sound (%s)", SDL_GetError());
    } else {
        SDL_ResumeAudioStreamDevice(s_Stream);
    }
}

void Host_AudioFrame(uint32_t cycles) {
    int bytes;

    Spu_Run(cycles);
    bytes = (int)(Spu_FrameCount() * SPU_CHANNELS * sizeof(int16_t));
    if (s_Stream != NULL && SDL_GetAudioStreamQueued(s_Stream) < MAX_QUEUED_BYTES) {
        SDL_PutAudioStreamData(s_Stream, Spu_Frames(), bytes);
    } else {
        /* Headless, or too far ahead */
    }
    Spu_ClearFrames();
}
