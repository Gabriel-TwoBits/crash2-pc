#pragma once
// crash2-pc's stand-in for SwanStation's interrupt controller, for its SPU: the game waits on no SPU interrupt
class InterruptController
{
public:
  enum class IRQ
  {
    SPU
  };

  void InterruptRequest(IRQ irq) { (void)irq; }
};

extern InterruptController g_interrupt_controller;
