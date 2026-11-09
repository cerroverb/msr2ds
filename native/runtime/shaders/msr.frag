#version 450
// Samus Returns' single-screen compositor: pieces of the bottom screen over the game view, as the emulator-based
// build's vulkan_present_msr.frag (keep the two in step). Here the bottom screen is screen-oriented, so the hard edge
// bits are left, top, right, bottom.
//   style: x = opacity, y/z = luma key low/high edge (mode 3: y = gain), w = mode
//          (0 plain, 1 luma-keyed, 2 solid colour, 3 drawn over black)
//   flags.x: edge feather width, in percent of the quad (low byte); bits 8-11 keep the left, top, right and bottom
//            sides hard. flags.y: circular mask radius, in percent (0 = none)
// Also OpenGL's (present.vert), whose GLSL 3.30 takes const only with a constant initializer.
#ifndef VARYING
#define VARYING(n) layout(location = n)
#endif
#ifdef VULKAN
layout(push_constant) uniform P {
#else
layout(std140) uniform P {
#endif
    vec4 dst;
    vec4 src;
    vec4 style;
    ivec4 flags;
    vec4 color;
} p;
#ifdef VULKAN
layout(set = 0, binding = 0)
#endif
uniform sampler2D tex;   // the bottom screen, linear, clamp to edge
VARYING(0) in vec2 uv;
VARYING(1) in vec2 local;
layout(location = 0) out vec4 o;

float Feather(vec2 l) {
    float width = float(p.flags.x & 0xFF) / 100.0;
    if (width <= 0.0) return 1.0;
    vec4 hard = vec4(notEqual(ivec4(p.flags.x) & ivec4(0x100, 0x200, 0x400, 0x800), ivec4(0)));
    vec4 edge = vec4(l, 1.0 - l) + hard;
    return smoothstep(0.0, width, min(edge.x, edge.z)) * smoothstep(0.0, width, min(edge.y, edge.w));
}

float Circle(vec2 l) {
    if (p.flags.y <= 0) return 1.0;
    float radius = float(p.flags.y) / 100.0;
    return 1.0 - smoothstep(radius - 0.04, radius, length(l - 0.5));
}

void main() {
    float opacity = p.style.x;
    int mode = int(p.style.w + 0.5);
    if (mode == 2) {
        o = vec4(p.color.rgb, p.color.a * opacity * Feather(local));
        return;
    }
    vec4 screen = texture(tex, uv);
    float alpha = Feather(local) * Circle(local);
    if (mode == 3) {
        // Drawn over black: each pixel keeps the light it adds and hides what is behind it by its brightest channel
        // raised to 0.6; the gain brings back text the game has faded (the title's pulsing prompt).
        vec3 lit = min(screen.rgb * p.style.y, vec3(1.0));
        float coverage = max(lit.r, max(lit.g, lit.b));
        float cover = pow(coverage, 0.6);
        vec3 rgb = cover > 0.0 ? lit / cover : vec3(0.0);
        o = vec4(rgb, alpha * cover * opacity);
        return;
    }
    if (mode == 1) {
        // The bottom screen's dark blue backdrop fades out; rules, digits and icons stay.
        float brightness = max(screen.r, max(screen.g, screen.b));
        alpha *= smoothstep(p.style.y, p.style.z, brightness);
    }
    o = vec4(screen.rgb, alpha * opacity);
}
