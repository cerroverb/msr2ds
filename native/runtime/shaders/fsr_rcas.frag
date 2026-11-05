#version 450
#extension GL_GOOGLE_include_directive : require
// FSR 1 RCAS (AMD FidelityFX, MIT, shaders/fsr/): contrast-adaptive sharpening of the EASU output, drawn into the window.
layout(push_constant) uniform P { vec4 dst; vec4 src; ivec4 mode; vec4 extra; } p;   // extra.x = sharpness (stops, 0 = most), extra.zw = output origin
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) out vec4 o;
#define A_GPU 1
#define A_GLSL 1
#include "fsr/ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 q) { return texelFetch(tex, q, 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "fsr/ffx_fsr1.h"
void main() {
    AU4 con; FsrRcasCon(con, p.extra.x);
    AF1 r, g, b; FsrRcasF(r, g, b, AU2(gl_FragCoord.xy - p.extra.zw), con);
    o = vec4(r, g, b, 1.0);
}
