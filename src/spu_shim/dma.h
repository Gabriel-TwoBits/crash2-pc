#pragma once
// crash2-pc's stand-in for SwanStation's DMA controller, for its SPU: the host writes the SPU's RAM directly
class DMA
{
public:
  enum class Channel
  {
    SPU
  };

  void SetRequest(Channel channel, bool request) { (void)channel; (void)request; }
};

extern DMA g_dma;
