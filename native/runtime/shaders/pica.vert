#version 450
// Vertices arrive already shaded by the CPU (PICA vertex shader interpreter): clip-space position plus the
// fixed-function outputs. PICA clips z to [-w, 0]; Vulkan clips to [0, w], so z is negated.
layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec4 a_uv01;
layout(location = 3) in vec3 a_uv2tw;
layout(location = 4) in vec4 a_quat;
layout(location = 5) in vec3 a_view;
layout(location = 0) out vec4 v_col;
layout(location = 1) out vec4 v_uv01;
layout(location = 2) out vec3 v_uv2tw;
layout(location = 3) out vec4 v_quat;
layout(location = 4) out vec3 v_view;
void main() {
    gl_Position = vec4(a_pos.x, a_pos.y, -a_pos.z, a_pos.w);
    v_col = a_col; v_uv01 = a_uv01; v_uv2tw = a_uv2tw; v_quat = a_quat; v_view = a_view;
}
