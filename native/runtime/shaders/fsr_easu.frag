#version 450
#ifdef VULKAN
#extension GL_GOOGLE_include_directive : require
#endif
// FSR 1 EASU (AMD FidelityFX, MIT, shaders/fsr/): edge-adaptive upscale of one screen into an image of the output size.
// Also OpenGL's (video_gl.c, GLSL 4.00: textureGather), with the headers put in place of the #include lines (CMake).
#ifdef VULKAN
layout(push_constant) uniform P { vec4 dst; vec4 src; ivec4 mode; vec4 extra; } p;   // src.xy = input size, extra.xy = output size
layout(set = 0, binding = 0)
#else
layout(std140) uniform P { vec4 dst; vec4 src; ivec4 mode; vec4 extra; } p;
#endif
uniform sampler2D tex;
layout(location = 0) out vec4 o;
#define A_GPU 1
#define A_GLSL 1
#include "fsr/ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 q) { return textureGather(tex, q, 0); }
AF4 FsrEasuGF(AF2 q) { return textureGather(tex, q, 1); }
AF4 FsrEasuBF(AF2 q) { return textureGather(tex, q, 2); }
#include "fsr/ffx_fsr1.h"
void main() {
    AU4 c0, c1, c2, c3;
    FsrEasuCon(c0, c1, c2, c3, p.src.x, p.src.y, p.src.x, p.src.y, p.extra.x, p.extra.y);
    AF3 c; FsrEasuF(c, AU2(gl_FragCoord.xy), c0, c1, c2, c3);
    o = vec4(c, 1.0);
}
