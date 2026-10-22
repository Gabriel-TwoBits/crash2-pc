#include "spu_host.h"

#include <cstdio>
#include <cstdlib>

#include "cdrom.h"
#include "dma.h"
#include "host_interface.h"
#include "interrupt_controller.h"
#include "spu.h"
#include "system.h"

extern "C" {
#include "host_log.h"
#include "psyq/host_psyq.h"
}

/* The stand-ins' objects (spu_shim/) and SwanStation's SPU (its g_spu) */
Settings g_settings;
CDROM g_cdrom;
DMA g_dma;
InterruptController g_interrupt_controller;
static HostInterface s_HostInterface;
HostInterface* g_host_interface = &s_HostInterface;
extern SPU g_spu;

static constexpr uint32_t SPU_BASE = 0x1F801C00;

/* CRASH2_SPU_LOG=FILE: each register write as a line of the vertical blank, the address and the value, in
 * tools/spu_log.lua's format, for tools/compare_spu.py */
static constexpr const char* LOG_VARIABLE = "CRASH2_SPU_LOG";
static FILE* s_Log;

extern "C" void Spu_Init(void)
{
  const char* log = getenv(LOG_VARIABLE);

  g_spu.Initialize();
  if (log == nullptr)
  {
    return;
  }
  else
  {
    s_Log = fopen(log, "w");
  }
  if (s_Log == nullptr)
  {
    Host_Fatal("%s: can't write %s", LOG_VARIABLE, log);
  }
  else
  {
    /* Written as the SPU is */
  }
}

extern "C" void Spu_WriteRegister(uint32_t address, uint16_t value)
{
  if (s_Log != nullptr)
  {
    fprintf(s_Log, "%d %08x %04x\n", Host_Blanks(), address, value);
  }
  else
  {
    /* Not logged */
  }
  g_spu.WriteRegister(address - SPU_BASE, value);
}

extern "C" uint16_t Spu_ReadRegister(uint32_t address)
{
  return g_spu.ReadRegister(address - SPU_BASE);
}

extern "C" void Spu_WriteRam(uint32_t address, const void* data, uint32_t bytes)
{
  g_spu.WriteRAM(address, data, bytes);
}

extern "C" void Spu_Run(uint32_t cycles)
{
  g_spu.AddTicks(static_cast<TickCount>(cycles));
  g_spu.GeneratePendingSamples();
}

extern "C" const int16_t* Spu_Frames(void)
{
  return g_host_interface->GetAudioStream()->Frames();
}

extern "C" uint32_t Spu_FrameCount(void)
{
  return g_host_interface->GetAudioStream()->Count();
}

extern "C" void Spu_ClearFrames(void)
{
  g_host_interface->GetAudioStream()->Clear();
}
