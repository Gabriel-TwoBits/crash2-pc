#pragma once
// crash2-pc's stand-in for SwanStation's CPU, for its SPU: no DMA takes bus time here
#include "types.h"

namespace CPU {
inline TickCount GetPendingTicks()
{
  return 0;
}
} // namespace CPU
