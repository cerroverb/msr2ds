#version 450
// Presentation: one textured quad per screen (or overlay), placed by push constants in normalized device coordinates.
// Also OpenGL's (video_gl.c): its own first lines replace the #version line and define VARYING empty, and the push
// constants are a uniform buffer there.
#ifndef VARYING
#define VARYING(n) layout(location = n)
#endif
#ifdef VULKAN
layout(push_constant) uniform P {
#else
layout(std140) uniform P {
#endif
    vec4 dst;      // x0, y0, x1, y1 in NDC
    vec4 src;      // texture width, height, output pixels per texel x, y
    ivec4 mode;    // x = filter (0 nearest, 1 linear, 2 sharp bilinear, 4 FXAA), y = alpha blend, z = 1: rotate (see below)
    vec4 extra;    // filter parameters (RCAS sharpness in x)
} p;
VARYING(0) out vec2 uv;
void main() {
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));   // triangle strip: (0,0) (1,0) (0,1) (1,1)
    uv = p.mode.z == 1 ? vec2(1.0 - c.y, 1.0 - c.x) : c;   // 1: an LCD-framebuffer-oriented image (240 lines per screen column) -> screen
    gl_Position = vec4(mix(p.dst.xy, p.dst.zw, c), 0.0, 1.0);
}
