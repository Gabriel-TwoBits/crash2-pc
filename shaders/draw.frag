#version 450

// A primitive's pixel (src/render.c): its texel read from VRAM as the GPU reads it (a 4- or 8-bit index into its
// CLUT, or a 15-bit colour), modulated by the vertex colour, in true colour. A semi-transparent primitive's blending is
// its pipeline's; the shader scales the colour for it

layout(location = 0) noperspective in vec4 v_colour;
layout(location = 1) noperspective in vec2 v_texcoord;
layout(location = 2) flat in uint v_texture;
layout(location = 3) flat in uint v_limits;
layout(location = 4) flat in uint v_window;
layout(location = 5) flat in uint v_flags;

layout(set = 2, binding = 0) uniform usampler2D u_vram;

layout(location = 0) out vec4 o_colour;

// v_flags' bits (src/render.c's)
const uint TEXTURED = 1u;
const uint RAW = 2u;
const uint ABSOLUTE = 4u;
const uint OPAQUE_TEXELS = 8u;
const uint BLENDED_TEXELS = 16u;
const uint HALF = 32u;
const uint QUARTER = 64u;

const ivec2 VRAM_MASK = ivec2(1023, 511);
const uint SEMI_TRANSPARENT = 0x8000u;
// A 5-bit channel as the display shows it, shifted up by 3, and a vertex colour's 128 that leaves a texel as it is
const float FIVE_BIT_SCALE = 8.0 / 255.0;
const float MODULATION_UNIT = 128.0;
const float FIVE_BIT_MAX = 31.0;

uint vram(ivec2 position) {
    return texelFetch(u_vram, position & VRAM_MASK, 0).r;
}

vec3 channels(uint pixel) {
    return vec3(pixel & 31u, (pixel >> 5) & 31u, (pixel >> 10) & 31u);
}

// The texel at the texture coordinate (8 bits, wrapping): within the primitive's (its limits), the texture window
// applied, in the page
uint texel(vec2 texcoord) {
    ivec2 t = ivec2(floor(texcoord)) & ivec2(255);
    ivec2 low = ivec2(v_limits & 255u, (v_limits >> 8) & 255u);
    ivec2 high = ivec2((v_limits >> 16) & 255u, v_limits >> 24);
    ivec2 window_and = ivec2(v_window & 255u, (v_window >> 8) & 255u);
    ivec2 window_or = ivec2((v_window >> 16) & 255u, v_window >> 24);
    ivec2 page = ivec2((v_texture & 15u) * 64u, ((v_texture >> 4) & 1u) * 256u);
    uint depth = (v_texture >> 7) & 3u;
    ivec2 clut = ivec2(((v_texture >> 16) & 63u) * 16u, (v_texture >> 22) & 511u);

    t = (clamp(t, low, high) & window_and) | window_or;
    if (depth == 0u) {
        uint word = vram(page + ivec2(t.x >> 2, t.y));
        uint index = (word >> ((t.x & 3) * 4)) & 15u;

        return vram(clut + ivec2(index, 0));
    } else if (depth == 1u) {
        uint word = vram(page + ivec2(t.x >> 1, t.y));
        uint index = (word >> ((t.x & 1) * 8)) & 255u;

        return vram(clut + ivec2(index, 0));
    } else {
        return vram(page + t);
    }
}

void main() {
    vec3 colour;

    if ((v_flags & ABSOLUTE) != 0u) {
        colour = channels(vram(ivec2(floor(v_texcoord)))) * FIVE_BIT_SCALE;
    } else if ((v_flags & TEXTURED) != 0u) {
        uint pixel = texel(v_texcoord);
        bool blended = (pixel & SEMI_TRANSPARENT) != 0u;

        if (pixel == 0u || ((v_flags & OPAQUE_TEXELS) != 0u && blended) ||
            ((v_flags & BLENDED_TEXELS) != 0u && !blended)) {
            discard;
        } else if ((v_flags & RAW) != 0u) {
            colour = channels(pixel) * FIVE_BIT_SCALE;
        } else {
            vec3 modulated = channels(pixel) * floor(v_colour.rgb * 255.0 + 0.5) / MODULATION_UNIT;

            colour = min(modulated, vec3(FIVE_BIT_MAX)) * FIVE_BIT_SCALE;
        }
    } else {
        colour = v_colour.rgb;
    }
    if ((v_flags & HALF) != 0u) {
        o_colour = vec4(colour * 0.5, 0.5);
    } else if ((v_flags & QUARTER) != 0u) {
        o_colour = vec4(colour * 0.25, 1.0);
    } else {
        o_colour = vec4(colour, 1.0);
    }
}
