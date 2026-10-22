#version 450

layout(location = 0) in vec2 v_texcoord;

layout(set = 2, binding = 0) uniform sampler2D u_picture;

layout(location = 0) out vec4 o_colour;

void main() {
    o_colour = vec4(texture(u_picture, v_texcoord).rgb, 1.0);
}
