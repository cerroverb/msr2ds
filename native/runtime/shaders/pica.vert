#version 450
// Vertices arrive already shaded by the CPU (PICA vertex shader interpreter): clip-space position plus the
// fixed-function outputs. PICA clips z to [-w, 0]; Vulkan clips to [0, w], so z is negated.
// Also OpenGL's (pica_gl.c): its own first lines replace the #version line and define VARYING empty and CLIP_Z, which
// takes Vulkan's z to OpenGL's clip space ([-w, w], unless glClipControl gives Vulkan's), so the depth is the same.
#ifndef VARYING
#define VARYING(n) layout(location = n)
#endif
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec4 a_uv01;
layout(location = 3) in vec3 a_uv2tw;
layout(location = 4) in vec4 a_quat;
layout(location = 5) in vec3 a_view;
VARYING(0) out vec4 v_col;
VARYING(1) out vec4 v_uv01;
VARYING(2) out vec3 v_uv2tw;
VARYING(3) out vec4 v_quat;
VARYING(4) out vec3 v_view;
void main() {
#ifdef VULKAN
    gl_Position = vec4(a_pos.x, a_pos.y, -a_pos.z, a_pos.w);
#else
    gl_Position = vec4(a_pos.x, a_pos.y, CLIP_Z(-a_pos.z, a_pos.w), a_pos.w);
#endif
    v_col = a_col; v_uv01 = a_uv01; v_uv2tw = a_uv2tw; v_quat = a_quat; v_view = a_view;
}
