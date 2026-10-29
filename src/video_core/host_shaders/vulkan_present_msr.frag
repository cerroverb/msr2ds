// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Samus Returns single-screen compositor: draws pieces of the bottom screen over the game view.
// Uses the present push constants:
//   i_resolution: x = opacity, y/z = luma key low/high edge (mode 3: y = gain), w = mode
//                 (0 plain, 1 luma-keyed, 2 solid colour, 3 drawn over black)
//   o_resolution: modes 0/1: the piece's texture bounds (u0, v0, u1, v1), for edge feathering
//                 mode 2: the colour (texture coordinates then run 0..1 across the quad)
//   layer:        edge feather width, in percent of the piece (low byte); bits 8-11 keep the
//                 low x, low y, high x and high y sides hard (modes 0/1: x runs down the screen)
//   reverse_interlaced: circular mask radius, in percent of the piece (0 = none)

#version 450 core
#extension GL_ARB_separate_shader_objects : enable

layout (location = 0) in vec2 frag_tex_coord;
layout (location = 0) out vec4 color;

layout (push_constant, std140) uniform DrawInfo {
    mat4 modelview_matrix;
    vec4 i_resolution;
    vec4 o_resolution;
    int screen_id_l;
    int screen_id_r;
    int layer;
    int reverse_interlaced;
};

layout (set = 0, binding = 0) uniform sampler2D screen_textures[3];

vec4 GetScreen(int screen_id) {
#ifdef ARRAY_DYNAMIC_INDEX
    return texture(screen_textures[screen_id], frag_tex_coord);
#else
    switch (screen_id) {
    case 0:
        return texture(screen_textures[0], frag_tex_coord);
    case 1:
        return texture(screen_textures[1], frag_tex_coord);
    case 2:
        return texture(screen_textures[2], frag_tex_coord);
    }
#endif
}

float Feather(vec2 local) {
    const float width = float(layer & 0xFF) / 100.0;
    if (width <= 0.0) {
        return 1.0;
    }
    // Hard edges never fade: bits 8-11 for the low x, low y, high x and high y sides.
    const vec4 hard = vec4(notEqual(ivec4(layer) & ivec4(0x100, 0x200, 0x400, 0x800), ivec4(0)));
    const vec4 edge = vec4(local, 1.0 - local) + hard;
    return smoothstep(0.0, width, min(edge.x, edge.z)) * smoothstep(0.0, width, min(edge.y, edge.w));
}

float Circle(vec2 local) {
    if (reverse_interlaced <= 0) {
        return 1.0;
    }
    const float radius = float(reverse_interlaced) / 100.0;
    return 1.0 - smoothstep(radius - 0.04, radius, length(local - 0.5));
}

void main() {
    const float opacity = i_resolution.x;
    const int mode = int(i_resolution.w + 0.5);
    if (mode == 2) {
        color = vec4(o_resolution.rgb, o_resolution.a * opacity * Feather(frag_tex_coord));
        return;
    }
    const vec4 screen = GetScreen(screen_id_l);
    const vec2 local = (frag_tex_coord - o_resolution.xy) / (o_resolution.zw - o_resolution.xy);
    float alpha = Feather(clamp(local, 0.0, 1.0)) * Circle(clamp(local, 0.0, 1.0));
    if (mode == 3) {
        // Drawn over black (the title menus without their backdrop): each pixel keeps the light it
        // adds, and hides what is behind it by its brightest channel raised to 0.6, so grey icons
        // stay solid and the dark slot fills read as tinted glass. Black stays clear. The gain
        // brings back text the game has faded (the title's pulsing prompt).
        const vec3 lit = min(screen.rgb * i_resolution.y, vec3(1.0));
        const float coverage = max(lit.r, max(lit.g, lit.b));
        const float cover = pow(coverage, 0.6);
        const vec3 rgb = cover > 0.0 ? lit / cover : vec3(0.0);
        color = vec4(rgb, alpha * cover * opacity);
        return;
    }
    if (mode == 1) {
        // The bottom screen's backdrop is a dark blue; the HUD's rules, digits and icons are
        // bright. Fade the backdrop out so the elements float over the game like a native HUD.
        const float brightness = max(screen.r, max(screen.g, screen.b));
        alpha *= smoothstep(i_resolution.y, i_resolution.z, brightness);
    }
    color = vec4(screen.rgb, alpha * opacity);
}
