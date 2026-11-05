#version 450
// HDR output (video.c): the game's picture, drawn in SDR as usual, with its highlights (lights, beams, the glow) expanded
// towards the display's peak, and the interface layer over it at paper white (the HUD pieces, menus and the settings
// stay as bright as Windows shows SDR content). Written to an HDR10 swapchain (PQ, BT.2020) or an scRGB one (linear
// BT.709, 1.0 = 80 nits).
layout(push_constant) uniform P {
    vec4 dst;      // present.vert's quad: the whole target
    vec4 src;
    ivec4 mode;    // x: 1 HDR10, 2 scRGB
    vec4 extra;    // x: paper white (nits), y: peak (nits), z: the knee (SDR level where the expansion starts)
} p;
layout(set = 0, binding = 0) uniform sampler2D game;   // the presentation's target (SDR, gamma-encoded)
layout(set = 0, binding = 1) uniform sampler2D ui;     // the interface layer: colour premultiplied by coverage, gamma-encoded
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

// SDR content was made for a gamma 2.2 display (as the 3DS's LCDs and most monitors show it)
vec3 linear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }

// luminance above the knee rises smoothly to peak / paper at SDR white (slope 1 at the knee); the colour keeps its hue
vec3 expand(vec3 c) {
    float r = p.extra.y / p.extra.x, k = p.extra.z;
    float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
    if (y <= k || r <= 1.0) return c;
    float t = min((y - k) / (1.0 - k), 1.0);
    return c * ((y + (r - 1.0) * t * t) / y);
}

vec3 pq(vec3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

void main() {
    vec4 u = texture(ui, uv);
    // the highlights expand where the interface leaves the picture showing: under its cards and dims (the file select's,
    // the pause menu's; from a quarter of coverage, smoothly below) the picture stays at its SDR level, so they darken
    // it as in SDR (2026-11-03, the user: the file select's background was "not contrasted enough"; expanded first, a
    // 25% see-through card still showed it at ~90% of paper white)
    vec3 lin = linear(texture(game, uv).rgb);
    vec3 g = mix(expand(lin), lin, smoothstep(0.0, 0.25, u.a)) * p.extra.x;
    vec3 ul = u.a > 0.0 ? linear(u.rgb / u.a) * p.extra.x : vec3(0.0);
    vec3 nits = g * (1.0 - u.a) + ul * u.a;
    if (p.mode.x == 1) {
        const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164,    // columns: BT.709 R, G, B in BT.2020
                                 0.3293, 0.9195, 0.0880,
                                 0.0433, 0.0114, 0.8956);
        o = vec4(pq(to2020 * nits), 1.0);
    } else {
        o = vec4(nits / 80.0, 1.0);
    }
}
