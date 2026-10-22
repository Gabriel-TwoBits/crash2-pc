#version 450

// The GPU's primitives at the internal resolution (src/render.c): positions in VRAM's pixels, the target the whole
// VRAM; the attributes passed on as the PlayStation's GPU steps them, without perspective

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_colour;
layout(location = 2) in vec2 a_texcoord;
layout(location = 3) in uint a_texture;
layout(location = 4) in uint a_limits;
layout(location = 5) in uint a_window;
layout(location = 6) in uint a_flags;

// x: half a target pixel in VRAM's pixels, which moves the primitives so the target samples at the corners of the
// PlayStation's pixels, as its GPU does, rather than at their centres
layout(set = 1, binding = 0) uniform Params {
    vec4 u_params;
};

layout(location = 0) noperspective out vec4 v_colour;
layout(location = 1) noperspective out vec2 v_texcoord;
layout(location = 2) flat out uint v_texture;
layout(location = 3) flat out uint v_limits;
layout(location = 4) flat out uint v_window;
layout(location = 5) flat out uint v_flags;

const vec2 VRAM_HALF_SIZE = vec2(512.0, 256.0);

void main() {
    vec2 position = (a_position + u_params.xx) / VRAM_HALF_SIZE - 1.0;

    gl_Position = vec4(position.x, -position.y, 0.0, 1.0);
    v_colour = a_colour;
    v_texcoord = a_texcoord;
    v_texture = a_texture;
    v_limits = a_limits;
    v_window = a_window;
    v_flags = a_flags;
}
