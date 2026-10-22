#include "host.h"

/* The GTE's 32 data and 32 control registers (host.h's register moves).
 * TRADEOFF: plain words; the GTE's own reads and writes (sign extension of the 16-bit registers, the screen FIFO) come
 * with the picture (docs/ROADMAP.md, milestone 5) */
#define GTE_HOST_REGISTERS 64

unsigned int g_HostGte[GTE_HOST_REGISTERS];
