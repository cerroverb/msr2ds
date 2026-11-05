#version 450
// Presentation filters for the 3DS screens: nearest, linear, sharp bilinear (crisp pixels without shimmering at
// non-integer scales), FXAA (then bilinear), and the overlay (alpha). FSR is two other shaders (fsr_easu/fsr_rcas.frag).
layout(push_constant) uniform P {
    vec4 dst;
    vec4 src;      // texture width, height, output pixels per texel x, y
    ivec4 mode;
    vec4 extra;
} p;
layout(set = 0, binding = 0) uniform sampler2D tex;   // linear sampler, clamp to edge
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

// FXAA: luma edges from the four diagonal neighbours give a blur direction; two taps along it (inner) or four (outer),
// keeping the outer result unless it leaves the local luma range. Runs in source-texel space, so it smooths the
// game's own aliased edges before the upscale.
vec3 fxaa(vec2 q, vec2 rcp) {
    const vec3 L = vec3(0.299, 0.587, 0.114);
    vec3 m = texture(tex, q).rgb;
    float nw = dot(texture(tex, q + vec2(-1.0, -1.0) * rcp).rgb, L), ne = dot(texture(tex, q + vec2(1.0, -1.0) * rcp).rgb, L);
    float sw = dot(texture(tex, q + vec2(-1.0, 1.0) * rcp).rgb, L), se = dot(texture(tex, q + vec2(1.0, 1.0) * rcp).rgb, L), lm = dot(m, L);
    float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
    if (hi - lo < max(0.0312, hi * 0.125)) return m;                 // flat area: leave it
    vec2 dir = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * 0.03125, 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * rcp;
    vec3 a = 0.5 * (texture(tex, q + dir * (1.0 / 3.0 - 0.5)).rgb + texture(tex, q + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(tex, q - dir * 0.5).rgb + texture(tex, q + dir * 0.5).rgb);
    float lb = dot(b, L);
    return (lb < lo || lb > hi) ? a : b;
}

void main() {
    vec2 size = p.src.xy, t = uv * size;
    vec2 q;
    if (p.mode.x == 0) q = (floor(t) + 0.5) / size;                              // nearest
    else if (p.mode.x == 2) {                                                    // sharp bilinear
        vec2 scale = max(p.src.zw, vec2(1.0)), region = 0.5 - 0.5 / scale;
        vec2 base = floor(t - 0.5), f = t - 0.5 - base;
        f = clamp((f - region) / (1.0 - 2.0 * region), 0.0, 1.0);
        q = (base + 0.5 + f) / size;
    } else q = uv;                                                               // linear
    o = p.mode.x == 4 ? vec4(fxaa(uv, 1.0 / size), 1.0) : texture(tex, q);
    if (p.mode.y == 0) o.a = 1.0;
}
