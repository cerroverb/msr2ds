#version 450
// Samus Returns' single-screen compositor (msr/compositor.cpp): one quad, a region of the bottom screen or a fill.
layout(push_constant) uniform P {
    vec4 dst;      // x0, y0, x1, y1 in NDC
    vec4 src;      // the bottom screen's u0, v0, u1, v1 (screen-oriented)
    vec4 style;    // opacity, key low (mode 3: gain), key high, mode
    ivec4 flags;   // x = feather | hard edges << 8, y = circle radius (percent)
    vec4 color;    // fill colour (mode 2)
} p;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec2 local;
void main() {
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));   // triangle strip: (0,0) (1,0) (0,1) (1,1)
    uv = mix(p.src.xy, p.src.zw, c);
    local = c;
    gl_Position = vec4(mix(p.dst.xy, p.dst.zw, c), 0.0, 1.0);
}
