#include "gpu_raster.h"

#include "core/gpu_sw_backend.h"

#include <cstddef>
#include <new>

/* SwanStation's command structures end in a vertex array of their own length: one buffer each, large enough for the
 * longest primitive gpu.c passes (a quad, a polyline's vertices) */
static constexpr int MAX_VERTICES = 256;
/* E6's bits as SwanStation's parameters hold them */
static constexpr uint32_t MASK_SET = 1;
static constexpr uint32_t MASK_CHECK = 2;
static constexpr uint32_t COLOUR_MASK = 0xFFFFFF;
/* E2's fields, 5 bits each in units of 8 texels: the mask's x and y, then the offset's */
static constexpr uint32_t WINDOW_FIELD_MASK = 0x1F;
static constexpr uint32_t WINDOW_MASK_Y_SHIFT = 5;
static constexpr uint32_t WINDOW_OFFSET_X_SHIFT = 10;
static constexpr uint32_t WINDOW_OFFSET_Y_SHIFT = 15;
static constexpr uint32_t WINDOW_UNIT = 8;

static GPU_SW_Backend s_Backend;
alignas(std::max_align_t) static unsigned char
    s_Polygon[sizeof(GPUBackendDrawPolygonCommand) + MAX_VERTICES * sizeof(GPUBackendDrawPolygonCommand::Vertex)];
alignas(std::max_align_t) static unsigned char
    s_Line[sizeof(GPUBackendDrawLineCommand) + MAX_VERTICES * sizeof(GPUBackendDrawLineCommand::Vertex)];

static GPUBackendCommandParameters parameters(uint32_t mask)
{
  GPUBackendCommandParameters params;

  params.bits = 0;
  params.set_mask_while_drawing = (mask & MASK_SET) != 0;
  params.check_mask_before_draw = (mask & MASK_CHECK) != 0;
  return params;
}

/* E2's value as SwanStation's front end turns it into its texture window (GPU::SetTextureWindow) */
static GPUTextureWindow texture_window(uint32_t value)
{
  const uint32_t mask_x = value & WINDOW_FIELD_MASK;
  const uint32_t mask_y = (value >> WINDOW_MASK_Y_SHIFT) & WINDOW_FIELD_MASK;
  const uint32_t offset_x = (value >> WINDOW_OFFSET_X_SHIFT) & WINDOW_FIELD_MASK;
  const uint32_t offset_y = (value >> WINDOW_OFFSET_Y_SHIFT) & WINDOW_FIELD_MASK;
  GPUTextureWindow window;

  window.and_x = static_cast<uint8_t>(~(mask_x * WINDOW_UNIT));
  window.and_y = static_cast<uint8_t>(~(mask_y * WINDOW_UNIT));
  window.or_x = static_cast<uint8_t>((offset_x & mask_x) * WINDOW_UNIT);
  window.or_y = static_cast<uint8_t>((offset_y & mask_y) * WINDOW_UNIT);
  return window;
}

static void fill_draw(GPUBackendDrawCommand* cmd, const RasterState* state)
{
  cmd->params = parameters(state->mask);
  cmd->rc.bits = state->command;
  cmd->draw_mode.bits = state->draw_mode;
  cmd->palette.bits = state->palette;
  cmd->window = texture_window(state->texture_window);
}

extern "C" void Raster_Init(uint16_t* vram)
{
  s_Backend.SetVRAM(vram);
}

extern "C" void Raster_SetDrawingArea(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom)
{
  s_Backend.SetDrawingArea(Common::Rectangle<uint32_t>(left, top, right, bottom));
}

extern "C" void Raster_Polygon(const RasterState* state, int count, const RasterVertex* vertices)
{
  auto* cmd = new (s_Polygon) GPUBackendDrawPolygonCommand();

  fill_draw(cmd, state);
  cmd->num_vertices = static_cast<uint16_t>(count);
  for (int index = 0; index < count; index++)
  {
    cmd->vertices[index].x = vertices[index].x;
    cmd->vertices[index].y = vertices[index].y;
    cmd->vertices[index].color = vertices[index].color & COLOUR_MASK;
    cmd->vertices[index].texcoord = vertices[index].texcoord;
  }
  s_Backend.DrawPolygon(cmd);
}

extern "C" void Raster_Line(const RasterState* state, int count, const RasterVertex* vertices)
{
  auto* cmd = new (s_Line) GPUBackendDrawLineCommand();

  if (count > MAX_VERTICES)
  {
    count = MAX_VERTICES;
  }
  else
  {
    /* Within the buffer */
  }
  fill_draw(cmd, state);
  cmd->palette.bits = 0;
  cmd->num_vertices = static_cast<uint16_t>(count);
  for (int index = 0; index < count; index++)
  {
    cmd->vertices[index].Set(vertices[index].x, vertices[index].y, vertices[index].color & COLOUR_MASK);
  }
  s_Backend.DrawLine(cmd);
}

extern "C" void Raster_Rectangle(const RasterState* state, int32_t x, int32_t y, uint16_t width, uint16_t height,
                                 uint16_t texcoord)
{
  GPUBackendDrawRectangleCommand cmd;

  fill_draw(&cmd, state);
  cmd.color = state->command & COLOUR_MASK;
  cmd.x = x;
  cmd.y = y;
  cmd.width = width;
  cmd.height = height;
  cmd.texcoord = texcoord;
  s_Backend.DrawRectangle(&cmd);
}

extern "C" void Raster_Fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color)
{
  s_Backend.FillVRAM(x, y, width, height, color & COLOUR_MASK, parameters(0));
}

extern "C" void Raster_Copy(uint32_t from_x, uint32_t from_y, uint32_t to_x, uint32_t to_y, uint32_t width,
                            uint32_t height, uint32_t mask)
{
  s_Backend.CopyVRAM(from_x, from_y, to_x, to_y, width, height, parameters(mask));
}

extern "C" void Raster_Load(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint16_t* pixels,
                            uint32_t mask)
{
  s_Backend.UpdateVRAM(x, y, width, height, pixels, parameters(mask));
}
