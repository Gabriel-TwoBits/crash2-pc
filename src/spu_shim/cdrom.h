#pragma once
// crash2-pc's stand-in for SwanStation's CD-ROM, for its SPU: the game plays no CD audio
#include "types.h"
#include <tuple>

class CDROM
{
public:
  std::tuple<int16_t, int16_t> GetAudioFrame() { return std::make_tuple(int16_t(0), int16_t(0)); }
};

extern CDROM g_cdrom;
