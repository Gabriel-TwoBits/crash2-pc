#pragma once
// crash2-pc's stand-in for SwanStation's host interface, for its SPU: the audio stream spu_host.cpp collects
#include "libretro/libretro_audio_stream.h"

class HostInterface
{
public:
  LibretroAudioStream* GetAudioStream() { return &m_stream; }

private:
  LibretroAudioStream m_stream;
};

extern HostInterface* g_host_interface;
