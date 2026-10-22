#pragma once
// crash2-pc's stand-in for SwanStation's audio stream, for its SPU: a buffer of stereo frames the host takes
#include <cstdint>

class LibretroAudioStream
{
public:
  static constexpr uint32_t BUFFER_FRAMES = 4096;
  static constexpr uint32_t CHANNELS = 2;

  uint32_t GetBufferSize() const { return BUFFER_FRAMES; }

  void BeginWrite(int16_t** buffer, uint32_t* frames)
  {
    *buffer = &m_frames[m_count * CHANNELS];
    *frames = BUFFER_FRAMES - m_count;
  }

  void EndWrite(uint32_t frames) { m_count += frames; }

  /* The frames written so far, which the host then empties */
  const int16_t* Frames() const { return m_frames; }
  uint32_t Count() const { return m_count; }
  void Clear() { m_count = 0; }

private:
  int16_t m_frames[BUFFER_FRAMES * CHANNELS] = {};
  uint32_t m_count = 0;
};
