#pragma once
// crash2-pc's stand-in for SwanStation's system.h, for its SPU alone (third_party/swanstation/core/spu.cpp): the
// master clock, no overclock, and timing events the host drives (spu_host.cpp adds each frame's CPU cycles)
#include "types.h"
#include <memory>

namespace System {
inline constexpr TickCount MASTER_CLOCK = 44100 * 0x300;

inline TickCount ScaleTicksToOverclock(TickCount ticks)
{
  return ticks;
}
} // namespace System

struct Settings
{
  bool cpu_overclock_active = false;
  uint32_t cpu_overclock_numerator = 1;
  uint32_t cpu_overclock_denominator = 1;
};

extern Settings g_settings;

/* An event the SPU runs its work through: here, the cycles the host has added since it last ran */
class TimingEvent
{
public:
  using Callback = void (*)(void* param, TickCount ticks, TickCount ticks_late);

  TimingEvent(Callback callback, void* param, TickCount interval)
    : m_callback(callback), m_param(param), m_interval(interval)
  {
  }

  bool IsActive() const { return m_active; }
  void Activate() { m_active = true; }
  void Deactivate() { m_active = false; }
  void Schedule(TickCount ticks) { (void)ticks; m_active = true; }
  TickCount GetInterval() const { return m_interval; }
  void SetInterval(TickCount interval) { m_interval = interval; }
  TickCount GetTicksSinceLastExecution() const { return m_pending; }
  void AddTicks(TickCount ticks) { m_pending += ticks; }

  void InvokeEarly(bool force = false)
  {
    if (m_pending > 0 || force)
    {
      const TickCount ticks = m_pending;
      m_pending = 0;
      m_callback(m_param, ticks, 0);
    }
    else
    {
      /* Nothing pending */
    }
  }

private:
  Callback m_callback;
  void* m_param;
  TickCount m_interval;
  TickCount m_pending = 0;
  bool m_active = false;
};

namespace TimingEvents {
inline std::unique_ptr<TimingEvent> CreateTimingEvent(const char* name, TickCount period, TickCount interval,
                                                      TimingEvent::Callback callback, void* param, bool activate)
{
  (void)name;
  (void)period;
  std::unique_ptr<TimingEvent> event = std::make_unique<TimingEvent>(callback, param, interval);
  if (activate)
    event->Activate();
  else
    event->Deactivate();
  return event;
}
} // namespace TimingEvents
