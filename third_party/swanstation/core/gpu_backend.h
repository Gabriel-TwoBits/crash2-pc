#pragma once
// Modified for crash2-pc on 2026-10-22 (README.md): SwanStation's GPUBackend reduced to what GPU_SW_Backend uses: the VRAM it draws into (the host's, set
// by SetVRAM) and the drawing area, its commands called directly, with no command queue or GPU thread.
#include "gpu_types.h"

class GPUBackend
{
public:
  GPUBackend() = default;
  virtual ~GPUBackend() = default;

  ALWAYS_INLINE uint16_t* GetVRAM() const { return m_vram_ptr; }
  void SetVRAM(uint16_t* vram) { m_vram_ptr = vram; }
  void SetDrawingArea(const Common::Rectangle<uint32_t>& area) { m_drawing_area = area; }

  virtual bool Initialize(bool force_thread) { (void)force_thread; return true; }
  virtual void Reset(bool clear_vram) { (void)clear_vram; m_drawing_area = {}; }

  virtual void FillVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color, GPUBackendCommandParameters params) = 0;
  virtual void UpdateVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data,
                          GPUBackendCommandParameters params) = 0;
  virtual void CopyVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height,
                        GPUBackendCommandParameters params) = 0;
  virtual void DrawPolygon(const GPUBackendDrawPolygonCommand* cmd) = 0;
  virtual void DrawRectangle(const GPUBackendDrawRectangleCommand* cmd) = 0;
  virtual void DrawLine(const GPUBackendDrawLineCommand* cmd) = 0;
  virtual void FlushRender() = 0;

protected:
  uint16_t* m_vram_ptr = nullptr;
  Common::Rectangle<uint32_t> m_drawing_area{};
};
