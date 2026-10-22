#version 450

// The picture over the window's viewport (src/render.c): two triangles from the vertex index, the source rectangle
// (x, y, width, height, in the texture's 0 to 1) in the uniform

layout(set = 1, binding = 0) uniform Source {
    vec4 u_source;
};

layout(location = 0) out vec2 v_texcoord;

const vec2 CORNERS[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                               vec2(0.0, 1.0));

void main() {
    vec2 corner = CORNERS[gl_VertexIndex];

    gl_Position = vec4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    v_texcoord = u_source.xy + corner * u_source.zw;
}
