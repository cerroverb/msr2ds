// The single 16:9 screen on msr-native: the top screen fills the view and the bottom screen's HUD pieces, menus and
// prompts are composited onto it, as the emulator-based build does (video_core/renderer_vulkan/renderer_vulkan.cpp
// DrawMsrScreens and ConsumeMsrReadbacks; keep the two in step). The classifier and the shared state are src/msr's
// own (bottom_classifier.h, presentation.h). Differences: the bottom screen is classified the frame it is shown
// (the runtime decodes it on the CPU anyway, no readback delay), fades follow host time (the game runs in real
// time here), and the result is a list of quads video.c draws (msr.vert/msr.frag) instead of Vulkan calls.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "msr.h"
#include "msr/bottom_classifier.h"
#include "msr/presentation.h"

extern "C" int msr_overlay_visible(void);   // overlay_host.cpp
#include "msr/touch_nav.h"

namespace {

namespace P = MSR::Presentation;
namespace BC = MSR::BottomClassifier;
using s64 = long long;

struct Rect {
    float l = 0, t = 0, r = 0, b = 0;
    float W() const { return r - l; }
    float H() const { return b - t; }
};

struct Vec2 { float x, y; };
struct Vec3 { float r, g, b; };

struct Style {
    float opacity = 1.f;
    bool keyed = false;
    float key_low = 0.f, key_high = 0.f;
    int feather = 0, circle = 0;
    bool over_black = false;
    float gain = 1.f;
    bool hard_borders = false;
};

constexpr float MapWindowWidth = 120.f, MapWindowHeight = 80.f;
// Samus's marker on the HUD's map: where the game keeps it, and how far it may be from there or from its last place
// to be taken at once (it moves a map tile, 10 px, at a time)
constexpr Vec2 MapMiddle{125.f, 135.f};
constexpr float MarkerReach = 24.f;
constexpr float FadePerSecond = 9.f, PromptFadePerSecond = 6.f;

Rect Source(float x0, float y0, float x1, float y1) { return {x0 / 320.f, y0 / 240.f, x1 / 320.f, y1 / 240.f}; }
float Approach(float v, float target, float step) { return v < target ? std::min(target, v + step) : std::max(target, v - step); }

// renderer_vulkan.h's msr_ members
struct State {
    int candidate = -1, candidate_frames = 0;
    s64 candidate_since_us = 0;
    int page_candidate = -1, page_frames = 0;
    s64 page_since_us = 0;
    float panel_alpha = 0, hud_alpha = 0, prompt_alpha = 0;
    s64 last_us = 0;
    long long start_seen = -1;
    s64 pausing_until_us = 0, resuming_until_us = 0;
    bool paused = false;
    s64 prompt_since_us = 0, prompt_seen_us = 0;
    bool in_play = false;
    s64 hud_seen_us = -1'000'000'000;   // the HUD last showed (the pad's menus' layout, glyphs.cpp): not yet
    bool menu_icons = true;   // the icons in the menus' layout: the game starts in its menus
    float zoom = 1.f;
    float aspect = 0.f;   // the view's aspect, easing to the wanted one (ViewAspectWanted); 0: not yet
    Rect prompt_text, prompt_card, prompt_card_shown;
    s64 prompt_card_us = 0, prompt_tall_us = 0, prompt_foreign_us = 0;
    float prompt_fit = 0;
    float bottom_peak = 0;
    bool bottom_black = false;
    float gallery = 0;
    Vec2 map_marker{125.f, 135.f}, map_center{125.f, 135.f};
    Vec2 marker_far{};          // a marker found away from the last one (Consume), since marker_far_us (-1: none)
    s64 marker_far_us = -1;
    std::array<Vec3, 4> map_backdrop{};
    std::array<s64, 3> beam_seen_us{-1, -1, -1}, beam_empty_us{-1, -1, -1};
    bool beams_greyed = false;
    float beam_grey = 0;
    bool beams_held = false;   // R held: the beam slots as they were (msr_compose)
    s64 beams_up_us = -1;      // R came up then (-1: still held)
    unsigned buttons = 0;
    bool menu_asked = false;   // a Start press opened the menus over play: they show as usual until the HUD is back
    bool menu_beside = false;  // the menu showing over play came without one (Consume): beside the view
} st;

std::vector<MsrQuad> quads;
s64 t0_us = -1;

void Region(const Rect& src, float x, float y, float w, float h, const Style& s) {
    MsrQuad q{};
    q.dst[0] = x; q.dst[1] = y; q.dst[2] = w; q.dst[3] = h;
    q.src[0] = src.l; q.src[1] = src.t; q.src[2] = src.r; q.src[3] = src.b;
    q.style[0] = s.opacity;
    q.style[1] = s.over_black ? s.gain : s.key_low;
    q.style[2] = s.key_high;
    q.style[3] = s.over_black ? 3.f : s.keyed ? 1.f : 0.f;
    q.feather = s.feather;
    if (s.hard_borders) {   // msr.frag: bits 8-11 = left, top, right, bottom (screen-oriented, unlike the emulator's)
        q.feather |= (src.l <= 0.f ? 0x100 : 0) | (src.t <= 0.f ? 0x200 : 0) | (src.r >= 1.f ? 0x400 : 0) |
                     (src.b >= 1.f ? 0x800 : 0);
    }
    q.circle = s.circle;
    quads.push_back(q);
}

void Fill(float x, float y, float w, float h, float r, float g, float b, float a, int feather = 0) {
    MsrQuad q{};
    q.dst[0] = x; q.dst[1] = y; q.dst[2] = w; q.dst[3] = h;
    q.style[0] = 1.f; q.style[3] = 2.f;
    q.color[0] = r; q.color[1] = g; q.color[2] = b; q.color[3] = a;
    q.feather = feather;
    quads.push_back(q);
}

// ConsumeMsrReadbacks: classify the bottom screen, track the beam slots, the map marker and the prompt's text, and
// publish the state once it has held.
void Consume(const BC::Image& image, s64 now) {
    const auto result = BC::Classify(image);
    const int state = static_cast<int>(result.bottom);
    st.bottom_peak = static_cast<float>(BC::Peak(image)) / 255.f;
    st.bottom_black = BC::DarkFraction(image, 6) > .9f;
    if (result.page == P::Page::FileList || result.page == P::Page::FileOpen) {
        P::beam_slot.store(-1, std::memory_order_relaxed);
        P::beam_slots.store(0, std::memory_order_relaxed);
        st.beam_seen_us.fill(-1);
        st.beam_empty_us.fill(-1);
    }
    if (result.bottom == P::Bottom::Hud) {
        const auto held = [now](s64& since) {
            if (since < 0 || since > now) since = now;
            return now - since;
        };
        if (!st.beams_held) {   // (R held: the slots show the missiles; the beams are kept as they were)
            int owned = P::beam_slots.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < st.beam_seen_us.size(); ++i) {
                const int bit = 1 << i;
                if (result.beams.present & bit) {
                    st.beam_empty_us[i] = -1;
                    if (held(st.beam_seen_us[i]) >= 100'000) owned |= bit;
                } else {
                    st.beam_seen_us[i] = -1;
                    if (result.beams.greyed) st.beam_empty_us[i] = -1;
                    else if (held(st.beam_empty_us[i]) >= 1'000'000) owned &= ~bit;
                }
            }
            P::beam_slots.store(owned, std::memory_order_relaxed);
            // the lit slot, greyed too (Morph Ball: the beam can still be changed); unclear: as it was
            if (result.beams.lit >= 0) P::beam_slot.store(result.beams.lit, std::memory_order_relaxed);
            st.beams_greyed = result.beams.greyed;
        }
        if (result.marker.found) {
            // Samus's marker, not something else as red seen while it fades (2026-11-03: a red map pin, which the
            // classifier no longer counts): taken near where it was or the map's middle, where the game keeps it;
            // elsewhere (a new area, a teleport) once found there for 0.75 s without it showing near the last place
            const Vec2 m{result.marker.x, result.marker.y};
            const auto near = [&m](Vec2 p) { return std::hypot(m.x - p.x, m.y - p.y) <= MarkerReach; };
            bool take = st.hud_alpha <= 0.f || near(st.map_marker) || near(MapMiddle);
            if (!take) {
                if (st.marker_far_us < 0 || !near(st.marker_far)) {
                    st.marker_far = m;
                    st.marker_far_us = now;
                }
                take = now - st.marker_far_us >= 750'000;
            }
            if (take) {
                st.map_marker = m;
                st.marker_far_us = -1;
                if (st.hud_alpha <= 0.f) st.map_center = st.map_marker;
            }
        }
        const auto& [mx0, my0, mx1, my1] = BC::MapRect;
        const int half_w = static_cast<int>(MapWindowWidth) / 2, half_h = static_cast<int>(MapWindowHeight) / 2;
        const int wx0 = std::clamp(static_cast<int>(st.map_marker.x) - half_w, mx0, mx1 - 1);
        const int wx1 = std::clamp(static_cast<int>(st.map_marker.x) + half_w, mx0 + 1, mx1);
        const int wy0 = std::clamp(static_cast<int>(st.map_marker.y) - half_h, my0, my1 - 1);
        const int wy1 = std::clamp(static_cast<int>(st.map_marker.y) + half_h, my0 + 1, my1);
        const std::array<std::array<int, 4>, 4> strips{{{mx0, wy0, mx0 + 2, wy1},
                                                        {wx0, my0, wx1, my0 + 2},
                                                        {mx1 - 2, wy0, mx1, wy1},
                                                        {wx0, my1 - 2, wx1, my1}}};
        for (std::size_t i = 0; i < strips.size(); ++i) {
            const auto& [x0, y0, x1, y1] = strips[i];
            const auto [r, g, b] = BC::Backdrop(image, x0, y0, x1, y1);
            st.map_backdrop[i] = {r / 255.f, g / 255.f, b / 255.f};
        }
    }
    if (result.bottom == P::Bottom::Prompt) {
        const auto box = BC::BrightBox(image, 140);
        if (!box.Empty()) {
            const int x0 = std::min(box.x0, 320 - box.x1);
            st.prompt_text = {static_cast<float>(x0), static_cast<float>(box.y0), static_cast<float>(320 - x0),
                              static_cast<float>(box.y1)};
        }
    }
    if (result.bottom != P::Bottom::Prompt) {
        bool other = result.bottom == P::Bottom::Menu || result.bottom == P::Bottom::Hud;
        if (!other) {
            const auto content = BC::BrightBox(image, 4);
            other = !content.Empty() && (content.x0 == 0 || content.y0 == 0 || content.x1 == BC::Width ||
                                         content.y1 == BC::Height);
        }
        if (other) {
            st.prompt_foreign_us = now;
            st.prompt_alpha = 0.f;
        }
    }
    if (state == st.candidate) {
        ++st.candidate_frames;
    } else {
        st.candidate = state;
        st.candidate_frames = 1;
        st.candidate_since_us = now;
    }
    // The page holds the same way (unlike the emulator build, which takes each frame's): menus sliding in or out read
    // as no page for a few frames, and touch navigation then lost its buttons.
    const int page = static_cast<int>(result.page);
    if (page == st.page_candidate) {
        ++st.page_frames;
    } else {
        st.page_candidate = page;
        st.page_frames = 1;
        st.page_since_us = now;
    }
    bool held = st.candidate_frames >= 3 && now - st.candidate_since_us >= 80'000;
    // A menu over play that no Start press asked for (#12, #32). Play's menus open from Start (the pause menu); without
    // one, the bottom screen read as a menu was the HUD misread (picking things up flashed the whole map) or a screen of
    // the game's own over play (an elevator's map, whose panel covered the top screen's prompt). Such a menu shows only
    // once it has held 1.5 s, and then beside the view, as the gallery's panel (msr_compose), the view's prompts in sight.
    const int menu = static_cast<int>(P::Bottom::Menu);
    const bool menu_new = state == menu && P::bottom.load(std::memory_order_relaxed) != menu;
    bool beside = false;
    if (held && menu_new && st.in_play && !st.menu_asked) {
        const long long start_us = P::start_pressed_us.load(std::memory_order_relaxed);   // -1: none yet
        if (start_us >= 0 && now - start_us < 2'000'000) st.menu_asked = true;
        else if (now - st.candidate_since_us < 1'500'000) held = false;
        else beside = true;
    }
    if (held) {
        const bool page_held = st.page_frames >= 3 && now - st.page_since_us >= 80'000;
        static const bool trace = std::getenv("PWR_MSR_TRACE") != nullptr;   // PWR_MSR_TRACE: the published states
        if (trace && (P::bottom.load() != state || (page_held && P::page.load() != page)))
            std::fprintf(stderr, "msr: bottom %d page %d%s\n", state, page_held ? page : P::page.load(),
                         menu_new && beside ? " (no Start: beside the view)" : "");
        P::bottom.store(state, std::memory_order_relaxed);
        if (page_held) P::page.store(page, std::memory_order_relaxed);
        if (menu_new) st.menu_beside = beside;
        if (state != menu) st.menu_beside = false;
        if (state == static_cast<int>(P::Bottom::Hud)) st.menu_asked = false;
        P::menu_beside.store(st.menu_beside, std::memory_order_relaxed);
    }
}

// The view's aspect for a window: 16:9 with widescreen, else the top screen's 5:3. Ultrawide (2026-11-03): in play the
// view takes a wider window's aspect, up to 32:9; the world's squeeze (msr_world_scale) and the camera's field of view
// (camera_fov.cpp) follow it through P::view_aspect. The title menus, the gallery and the prologue stay 16:9: the game
// draws their scenes for its own view only, and their art (the title's logo) flat, as camera-facing quads without depth
// test, which the squeeze leaves as they are (they would stretch to the window's width).
// Fill 16:10 (2026-11-06, #38): in play the view also takes a taller window's aspect, down to 16:10, keeping 16:9's
// width and showing more above and below (P::ViewScaleV); the camera culls for that too.
float ViewAspectWanted(float ww, float wh) {
    if (!P::widescreen.load(std::memory_order_relaxed)) return P::NativeAspect;
    if (!st.in_play || wh <= 0.f) return P::WideAspect;
    const float lo = P::fill_16_10.load(std::memory_order_relaxed) ? P::TallAspect : P::WideAspect;
    const float hi = P::ultrawide.load(std::memory_order_relaxed) ? P::UltraAspect : P::WideAspect;
    return std::clamp(ww / wh, lo, hi);
}

// emu_window.cpp MsrSingleScreenLayout: the view (its aspect in effect, P::view_aspect) centred in the window, the panel
// (the bottom screen's 4:3) centred in it at panel_scale of its height.
void SingleLayout(float ww, float wh, Rect& top, Rect& panel) {
    const float aspect = P::view_aspect.load(std::memory_order_relaxed);   // width over height
    float w = ww, h = std::floor(ww / aspect);
    if (h > wh) { h = wh; w = std::floor(wh * aspect); }
    const float x = std::floor((ww - w) / 2), y = std::floor((wh - h) / 2);
    top = {x, y, x + w, y + h};
    const float ph = std::round(h * std::clamp(P::panel_scale.load(), .3f, 1.f));
    const float pw = std::round(ph * 320.f / 240.f);
    const float px = x + std::floor((w - std::min(pw, w)) / 2), py = y + std::floor((h - ph) / 2);
    panel = {px, py, px + pw, py + ph};
}

s64 NowUs(double now_s) {
    const s64 us = static_cast<s64>(now_s * 1e6);
    if (t0_us < 0) t0_us = us - 1;
    return us - t0_us;
}

} // namespace

extern "C" void msr_single_layout(float ww, float wh, float top[4], float panel[4]) {
    Rect t, p;
    SingleLayout(ww, wh, t, p);
    top[0] = t.l; top[1] = t.t; top[2] = t.W(); top[3] = t.H();
    panel[0] = p.l; panel[1] = p.t; panel[2] = p.W(); panel[3] = p.H();
}

extern "C" void msr_set_single(int on) { P::enabled.store(on != 0, std::memory_order_relaxed); }
extern "C" int msr_single(void) { return P::enabled.load(std::memory_order_relaxed); }
extern "C" int msr_panel_visible(void) { return P::panel_visible.load(std::memory_order_relaxed); }

extern "C" int msr_world_in_play(void) { return st.in_play ? 1 : 0; }
extern "C" int msr_beams_held(void) { return st.beams_held ? 1 : 0; }

extern "C" void msr_world_scale(float* sx, float* sy) {
    const float zoom = P::world_zoom.load(std::memory_order_relaxed);
    *sx = P::ViewScaleV() * zoom;   // clip x is the screen's vertical (the 3DS's top screen is turned)
    *sy = P::ViewScale() * zoom;
}

extern "C" void msr_compose(const uint8_t* bottom, double now_s, unsigned buttons, float ww, float wh,
                            MsrFrame* out) {
    quads.clear();
    const s64 now_us = NowUs(now_s);
    // HID Start rising: the game sees the press (the emulator's HID sets this as the game reads it)
    if ((buttons & 8) && !(st.buttons & 8)) {
        P::start_pressed_us.store(now_us, std::memory_order_relaxed);
        if (st.menu_beside) {   // Start over a menu beside the view: the pause menu it opens shows as usual
            st.menu_beside = false;
            st.menu_asked = true;
            P::menu_beside.store(false, std::memory_order_relaxed);
        }
    }
    st.buttons = buttons;
    // R held (missiles, Power Bombs): the game draws them in the beam slots, and the HUD keeps the beams as they were
    // (2026-11-03, the user: "it should just stay with whatever beam you were using"; the slots are the touch screen's
    // missile choice meanwhile, the game's own screen is not changed). video.c draws them from its copy of the bottom
    // screen made before R, and their classification waits. From a press while the HUD shows, until 350 ms after R
    // comes up: the beams come back on the next frame, but the lit one flashes and the Grapple slot fades in for ~20.
    if (buttons & 0x100) {
        if (static_cast<P::Bottom>(P::bottom.load(std::memory_order_relaxed)) == P::Bottom::Hud) st.beams_held = true;
        st.beams_up_us = -1;
    } else if (st.beams_held) {
        if (st.beams_up_us < 0) st.beams_up_us = now_us;
        else if (now_us - st.beams_up_us >= 350'000) st.beams_held = false;
    }
    if (bottom) Consume(BC::Image{bottom, false}, now_us);

    Rect layout_top, layout_panel;
    SingleLayout(ww, wh, layout_top, layout_panel);
    const auto state = static_cast<P::Bottom>(P::bottom.load(std::memory_order_relaxed));
    const bool forced = P::force_panel.load(std::memory_order_relaxed);
    const float seconds = st.last_us != 0 ? std::clamp(static_cast<float>(now_us - st.last_us) / 1e6f, 0.f, .1f) : 0.f;
    st.last_us = now_us;

    constexpr s64 transition_us = 1'500'000;
    const long long start_us = P::start_pressed_us.load(std::memory_order_relaxed);
    if (start_us != st.start_seen) {
        st.start_seen = start_us;
        if (state == P::Bottom::Hud && !forced) st.pausing_until_us = start_us + transition_us;
    }
    if (st.pausing_until_us != 0 && now_us >= st.pausing_until_us) st.pausing_until_us = 0;
    if (state == P::Bottom::Menu && st.pausing_until_us != 0) {
        st.pausing_until_us = 0;
        st.paused = true;
    }
    if (st.paused && state != P::Bottom::Menu) {
        st.paused = false;
        st.resuming_until_us = now_us + transition_us;
    }
    if (state == P::Bottom::Hud) st.resuming_until_us = 0;
    const bool pausing = st.pausing_until_us != 0;
    const bool resuming = now_us < st.resuming_until_us;

    if (state != P::Bottom::Prompt) st.prompt_since_us = now_us;
    else st.prompt_seen_us = now_us;
    // The pad's menus' layout (glyphs.cpp): its buttons choose and go back unless the HUD shows, from a menu at once or
    // anything else held 300 ms (2026-11-03, the user: "in game it also needs to change too": the game's dialogs over
    // play, "Save your progress?", show the emblem below). Its icons follow from a menu, or a prompt held 300 ms (the
    // title's), until the HUD shows; cutscenes, loading and those dialogs keep what was (their text names play's buttons).
    if (state == P::Bottom::Hud) {
        st.hud_seen_us = now_us;
        st.menu_icons = false;
    } else if (state == P::Bottom::Menu || (state == P::Bottom::Prompt && now_us - st.prompt_since_us >= 300'000)) {
        st.menu_icons = true;
    }
    msr_buttons_state(state != P::Bottom::Hud && (state == P::Bottom::Menu || now_us - st.hud_seen_us >= 300'000), st.menu_icons);
    const auto page = static_cast<P::Page>(P::page.load(std::memory_order_relaxed));
    if (state == P::Bottom::Hud) {
        st.in_play = true;
    } else if ((state == P::Bottom::Prompt && now_us - st.prompt_since_us >= 300'000) ||
               (state == P::Bottom::Menu &&
                (page == P::Page::FileList || page == P::Page::FileOpen || page == P::Page::Gallery))) {
        st.in_play = false;
    }
    // the view's aspect: eased between play's (ultrawide) and the menus' 16:9, applied from the next frame's layout
    const float aspect_target = ViewAspectWanted(ww, wh);
    if (st.aspect <= 0.f || std::abs(aspect_target - st.aspect) < .002f) st.aspect = aspect_target;
    else st.aspect += (aspect_target - st.aspect) * (1.f - std::exp(-8.f * seconds));
    P::view_aspect.store(st.aspect, std::memory_order_relaxed);
    const float zoom_target = st.in_play ? P::zoom.load(std::memory_order_relaxed) : 1.f;
    st.zoom += (zoom_target - st.zoom) * (1.f - std::exp(-8.f * seconds));
    if (std::abs(zoom_target - st.zoom) < .001f) st.zoom = zoom_target;
    P::world_zoom.store(st.zoom, std::memory_order_relaxed);

    const bool prompt_on = st.prompt_alpha > 0.f;
    const bool prompt = (state == P::Bottom::Prompt && (prompt_on || now_us - st.prompt_since_us >= 300'000)) ||
                        (prompt_on && state == P::Bottom::Logo && now_us - st.prompt_seen_us < 1'500'000);
    const bool show_hud = state == P::Bottom::Hud && !forced && !pausing;
    const bool show_panel = state == P::Bottom::Menu || forced;
    const bool show_prompt = prompt && !forced && !pausing && !resuming && now_us - st.prompt_foreign_us >= 300'000;
    const float fade = FadePerSecond * seconds;
    st.hud_alpha = Approach(st.hud_alpha, show_hud ? 1.f : 0.f, fade);
    st.panel_alpha = Approach(st.panel_alpha, show_panel ? 1.f : 0.f, fade);
    st.prompt_alpha = Approach(st.prompt_alpha, show_prompt ? 1.f : 0.f, PromptFadePerSecond * seconds);
    P::panel_visible = st.panel_alpha > 0.5f || st.prompt_alpha > 0.5f;

    // Gallery viewer, and a menu over play without Start (Consume): the view moves aside and the panel stands next to it.
    const bool gallery = state == P::Bottom::Menu && (page == P::Page::Gallery || st.menu_beside);
    st.gallery = Approach(st.gallery, gallery ? 1.f : 0.f, fade);
    const float ease = st.gallery * st.gallery * (3.f - 2.f * st.gallery);
    Rect view_top = layout_top, panel = layout_panel;
    if (ease > 0.f) {
        const Rect& full = layout_top;
        const float fx = full.l, fy = full.t, fw = full.W(), fh = full.H();
        const float gap = fh * .03f;
        const float vw = fw * .72f, vh = fh * .72f;
        const float vx = fx + gap, vy = fy + (fh - vh) / 2;
        const float room = fx + fw - (vx + vw) - 2 * gap;
        const float ph = std::min(room * .75f, vh), pw = ph / .75f;
        const float px = vx + vw + gap + (room - pw) / 2, py = fy + (fh - ph) / 2;
        const auto mix = [ease](const Rect& from, float x, float y, float w, float h) {
            const auto lerp = [ease](float a, float b) { return std::round(a + (b - a) * ease); };
            return Rect{lerp(from.l, x), lerp(from.t, y), lerp(from.r, x + w), lerp(from.b, y + h)};
        };
        view_top = mix(layout_top, vx, vy, vw, vh);
        panel = mix(layout_panel, px, py, pw, ph);
    }

    // the HUD at the view's corners; on a view wider than 16:9, within its middle 16:9 if the player asks
    float left = layout_top.l, width = layout_top.W();
    const float top_y = layout_top.t, height = layout_top.H();
    if (P::hud_within_16_9.load(std::memory_order_relaxed) && width > std::ceil(height * P::WideAspect)) {
        const float w = std::round(height * P::WideAspect);
        left += std::floor((width - w) / 2);
        width = w;
    }
    const float unit = height / 240.f * std::clamp(P::hud_scale.load(), .5f, 2.f);
    const float margin = 6.f * unit;

    if (st.hud_alpha > 0.f) {
        const float alpha = st.hud_alpha * std::clamp(P::hud_opacity.load(), 0.f, 1.f);
        Style hud;
        hud.opacity = alpha; hud.keyed = true; hud.key_low = .28f; hud.key_high = .5f; hud.feather = 6;
        hud.hard_borders = true;
        const auto piece = [&](float x0, float y0, float x1, float y1, float x, float y, float scale) {
            Region(Source(x0, y0, x1, y1), x, y, (x1 - x0) * unit * scale, (y1 - y0) * unit * scale, hud);
        };
        const float shade = .32f * alpha;

        constexpr float energy_scale = .8f, energy_height = 58.f;
        Fill(left + margin, top_y + margin - 2.f * unit * energy_scale, 230.f * unit * energy_scale,
             (energy_height + 4.f) * unit * energy_scale, 0, 0, 0, shade, 30);
        piece(0, 0, 250, energy_height, left + margin, top_y + margin, energy_scale);

        if (P::show_abilities) {
            st.beam_grey = Approach(st.beam_grey, st.beams_greyed ? 1.f : 0.f, 6.f * seconds);
            const auto keyed = [&](float low, float high, float grey_low, float grey_high) {
                Style s;
                s.opacity = alpha; s.keyed = true;
                s.key_low = low + (grey_low - low) * st.beam_grey;
                s.key_high = high + (grey_high - high) * st.beam_grey;
                s.circle = 38;
                return s;
            };
            const Style unlit_icon = keyed(.3f, .5f, .18f, .28f);
            const Style unlit_grapple = keyed(.3f, .5f, .39f, .47f);
            const Style lit_icon = keyed(.6f, .85f, .35f, .55f);
            const float size = 64.f * unit * .6f;
            const float y = top_y + margin + (energy_height + 2.f) * unit * energy_scale;
            const int lit = P::beam_slot.load(std::memory_order_relaxed);
            const int present = P::beam_slots.load(std::memory_order_relaxed);
            float x = left + margin;
            for (int i = 0; i < static_cast<int>(BC::BeamSlotRects.size()); ++i) {
                if (i > 0 && (present & (1 << i)) == 0) continue;
                if (i == lit) Fill(x, y, size, size, .35f, .75f, .95f, .4f * alpha * (1.f - .6f * st.beam_grey), 45);
                const auto& [x0, y0, x1, y1] = BC::BeamSlotRects[i];
                Region(Source(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1),
                              static_cast<float>(y1)),
                       x, y, size, size, i == lit ? lit_icon : i == 2 ? unlit_grapple : unlit_icon);
                quads.back().source = st.beams_held ? 1 : 0;   // R held: the beams before it (video.c)
                x += size + 4.f * unit;
            }
        }

        float map_x = left + width - margin, map_w = 0.f, map_bottom = top_y + margin;
        if (P::show_minimap) {
            const float scale = .675f * std::clamp(P::minimap_size.load(), .5f, 2.f);
            constexpr float source_w = MapWindowWidth, source_h = MapWindowHeight;
            const float w = source_w * unit * scale, h = source_h * unit * scale;
            const float x = left + width - margin - w, y = top_y + margin;
            const float opacity = alpha * std::clamp(P::minimap_opacity.load(), 0.f, 1.f);
            Style map;
            map.opacity = opacity;
            const Vec2 gap{st.map_marker.x - st.map_center.x, st.map_marker.y - st.map_center.y};
            if (std::abs(gap.x) + std::abs(gap.y) < .1f) {
                st.map_center = st.map_marker;
            } else {
                const float k = 1.f - std::exp(-12.f * seconds);
                st.map_center.x += gap.x * k;
                st.map_center.y += gap.y * k;
            }
            const float sx0 = st.map_center.x - source_w / 2, sy0 = st.map_center.y - source_h / 2;
            const auto& [mx0, my0, mx1, my1] = BC::MapRect;
            const float cx0 = std::max(sx0, static_cast<float>(mx0)), cy0 = std::max(sy0, static_cast<float>(my0));
            const float cx1 = std::min(sx0 + source_w, static_cast<float>(mx1));
            const float cy1 = std::min(sy0 + source_h, static_cast<float>(my1));
            const float k = w / source_w;
            const float ix = x + (cx0 - sx0) * k, iy = y + (cy0 - sy0) * k, iw = (cx1 - cx0) * k, ih = (cy1 - cy0) * k;
            const auto backdrop = [&](std::size_t edge, float bx, float by, float bw, float bh) {
                const auto& c = st.map_backdrop[edge];
                Fill(bx, by, bw, bh, c.r, c.g, c.b, opacity);
            };
            if (ix > x) backdrop(0, x, y, ix - x, h);
            if (iy > y) backdrop(1, ix, y, iw, iy - y);
            if (ix + iw < x + w) backdrop(2, ix + iw, y, x + w - (ix + iw), h);
            if (iy + ih < y + h) backdrop(3, ix, iy + ih, iw, y + h - (iy + ih));
            Region(Source(cx0, cy0, cx1, cy1), ix, iy, iw, ih, map);
            const float line = std::max(1.f, unit * .75f), fa = .8f * alpha;
            Fill(x - line, y - line, w + 2 * line, line, .35f, .75f, .95f, fa);
            Fill(x - line, y + h, w + 2 * line, line, .35f, .75f, .95f, fa);
            Fill(x - line, y, line, h, .35f, .75f, .95f, fa);
            Fill(x + w, y, line, h, .35f, .75f, .95f, fa);
            map_x = x;
            map_w = w;
            map_bottom = y + h + line;
        }

        if (P::show_counters) {
            const float natural = (33.f + 10.f + 48.f) * unit;
            const float fit = map_w > 0.f ? std::min(1.f, map_w / natural) : 1.f;
            const float dna_w = 33.f * unit * fit, gap = 10.f * unit * fit, total = natural * fit;
            const float h = 23.f * unit * fit;
            const float x = map_w > 0.f ? map_x + (map_w - total) / 2 : map_x - total;
            const float y = map_bottom + 6.f * unit;
            Fill(x - 8.f * unit, y - 3.f * unit, total + 16.f * unit, h + 6.f * unit, 0, 0, 0, shade, 30);
            Style counter;
            counter.opacity = alpha; counter.keyed = true; counter.key_low = .38f; counter.key_high = .58f;
            counter.feather = 6;
            Region(Source(64, 217, 97, 240), x, y, dna_w, h, counter);
            Region(Source(192, 217, 240, 240), x + dna_w + gap, y, 48.f * unit * fit, h, counter);
        }
    }

    if (st.prompt_alpha > 0.f) {
        const auto& typed = st.prompt_text;
        auto& card = st.prompt_card;
        const auto beyond = [](const Rect& a, const Rect& b) {
            return a.l < b.l - 1.f || a.t < b.t - 1.f || a.r > b.r + 1.f || a.b > b.b + 1.f;
        };
        if (st.prompt_card_us == 0 || std::abs(typed.t - card.t) > 4.f) {
            if (st.prompt_card_us == 0) st.prompt_card_shown = typed;
            card = typed;
            st.prompt_card_us = now_us;
        } else if (beyond(typed, card)) {
            card = {std::min(card.l, typed.l), std::min(card.t, typed.t), std::max(card.r, typed.r),
                    std::max(card.b, typed.b)};
            st.prompt_card_us = now_us;
        } else if (!beyond(card, typed) || now_us - st.prompt_card_us > 500'000) {
            card = typed;
            st.prompt_card_us = now_us;
        }
        const float pad_x = 12.f, pad_y = 7.f;
        const float panel_scale = panel.H() / 240.f;
        const float fit_scale = std::min({panel_scale, height * .52f / 180.f, width * .94f / 320.f});
        if (card.b - card.t > 26.f || height * .8f + (card.b + pad_y - 120.f) * panel_scale > height * .95f) {
            st.prompt_tall_us = now_us;
        }
        const bool tall = st.prompt_tall_us != 0 && now_us - st.prompt_tall_us < 5'000'000;
        st.prompt_fit = Approach(st.prompt_fit, tall ? 1.f : 0.f, 4.f * seconds);
        const float fit = st.prompt_fit * st.prompt_fit * (3.f - 2.f * st.prompt_fit);
        const float scale = panel_scale + (fit_scale - panel_scale) * fit;
        const float short_y = height * .8f - 120.f * panel_scale;
        const float fit_y = std::min(height * .8f, height * .95f - 90.f * fit_scale) - 126.f * fit_scale;
        const float w = 320.f * scale, h = 240.f * scale;
        const float x = left + (width - w) / 2, y = top_y + short_y + (fit_y - short_y) * fit;

        auto& shown = st.prompt_card_shown;
        const float e = std::min(1.f, 12.f * seconds);
        shown = {shown.l + (card.l - shown.l) * e, shown.t + (card.t - shown.t) * e, shown.r + (card.r - shown.r) * e,
                 shown.b + (card.b - shown.b) * e};
        const float card_alpha = std::clamp(P::menu_backdrop.load(std::memory_order_relaxed), 0.f, 1.f);
        Fill(x + (shown.l - pad_x) * scale, y + (shown.t - pad_y) * scale, (shown.r - shown.l + 2 * pad_x) * scale,
             (shown.b - shown.t + 2 * pad_y) * scale, .01f, .035f, .07f, card_alpha * st.prompt_alpha, 4);
        Style text;
        if (st.bottom_black) {
            const float peak = std::max(st.bottom_peak, .05f);
            text.opacity = st.prompt_alpha * (.7f + .3f * std::min(peak / .88f, 1.f));
            text.over_black = true;
            text.gain = std::clamp(.88f / peak, 1.f, 12.f);
        } else {
            text.opacity = st.prompt_alpha; text.keyed = true; text.key_low = .45f; text.key_high = .65f;
            text.feather = 4;
        }
        Region({0.f, 0.f, 1.f, 1.f}, x, y, w, h, text);
    } else {
        st.prompt_card_us = 0;
        if (now_us - st.prompt_tall_us >= 5'000'000) st.prompt_fit = 0.f;
    }

    if (st.panel_alpha > 0.f) {
        const bool see_through = P::backdrop_removed.load(std::memory_order_relaxed);
        const float card = std::clamp(P::menu_backdrop.load(std::memory_order_relaxed), 0.f, 1.f);
        // the view darkens behind the menus: the pause menu's by 45%, the title menus' (the file select) by 25-60% as
        // the Title menu background setting says (2026-11-03, the user: the file select's background "needs to be
        // darkened"; it was 20%)
        Fill(left, top_y, width, height, 0, 0, 0, (see_through ? .25f + .35f * card : .45f) * st.panel_alpha * (1.f - ease));
        if (see_through) {
            const float grow = panel.H() * .03f;
            Fill(panel.l - grow, panel.t - grow, panel.W() + 2 * grow, panel.H() + 2 * grow, .01f, .035f, .07f,
                 card * st.panel_alpha, 4);
        }
        Style full;
        full.opacity = st.panel_alpha;
        full.over_black = see_through;
        Region({0.f, 0.f, 1.f, 1.f}, panel.l, panel.t, panel.W(), panel.H(), full);

        // Touch navigation: a glowing frame around the focused button, or a crosshair cursor.
        if (P::touch_mode.load(std::memory_order_relaxed)) {
            const float sx = panel.W() / 320.f, sy = panel.H() / 240.f, px = panel.l, py = panel.t;
            const float a = .95f * st.panel_alpha, line = std::max(2.f, 1.25f * sx);
            const int x0 = P::touch_x0, y0 = P::touch_y0, x1 = P::touch_x1, y1 = P::touch_y1;
            if (x1 > x0 && y1 > y0) {
                const float fx = px + x0 * sx - line, fy = py + y0 * sy - line, fw = (x1 - x0) * sx + 2 * line, fh = (y1 - y0) * sy + 2 * line;
                Fill(fx, fy, fw, fh, 1.f, .78f, .2f, .18f * st.panel_alpha);
                Fill(fx, fy, fw, line, 1.f, .78f, .2f, a);
                Fill(fx, fy + fh - line, fw, line, 1.f, .78f, .2f, a);
                Fill(fx, fy, line, fh, 1.f, .78f, .2f, a);
                Fill(fx + fw - line, fy, line, fh, 1.f, .78f, .2f, a);
            } else {
                const float cx = px + P::touch_x * sx, cy = py + P::touch_y * sy, arm = 7.f * sx;
                Fill(cx - arm, cy - line / 2, 2 * arm, line, 1.f, .78f, .2f, a);
                Fill(cx - line / 2, cy - arm, line, 2 * arm, 1.f, .78f, .2f, a);
            }
        }
    }

    out->top[0] = view_top.l; out->top[1] = view_top.t; out->top[2] = view_top.W(); out->top[3] = view_top.H();
    out->panel[0] = panel.l; out->panel[1] = panel.t; out->panel[2] = panel.W(); out->panel[3] = panel.H();
    out->quads = quads.data();
    out->count = static_cast<int>(quads.size());
}

// vk_rasterizer.cpp SkipMsrBackdrop: the title menus open the bottom screen's frame with two full-screen quads (the
// blue scanlined fill, then the emblem) from a 1024x512 I8 atlas; left out, the menus stand on black, which the
// compositor turns into transparency. Called for each draw into the bottom screen's 240x320 target; a display
// transfer starts the next frame (msr_bottom_frame).
namespace {
int bottom_draw = -1;
bool backdrop = false;
} // namespace

extern "C" void msr_bottom_frame(void) { bottom_draw = -1; }

extern "C" int msr_skip_bottom_draw(int quad_backdrop) {
    if (++bottom_draw > 1) return 0;
    if (bottom_draw == 0) {
        backdrop = quad_backdrop != 0;
        P::backdrop_removed.store(backdrop, std::memory_order_relaxed);
    }
    return backdrop && quad_backdrop;
}

// input_bridge.cpp CycleBeam / ToggleMorphBall: the game switches beams and morphs only by touch, so RT taps the next
// beam slot the HUD shows and LT the minimap (one tap morphs, the next unmorphs), only while the HUD shows. A tap
// holds 100 ms; the bottom screen shows it a few frames later, so for 500 ms the next RT goes on from its target.
namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point tap_until{}, beam_settled{};
int tap_x = 0, tap_y = 0, beam_target = -1;
} // namespace

extern "C" void msr_trigger(int which) {
    static const bool trace = std::getenv("PWR_INPUT_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "msr: trigger %d (bottom %d, slots %d lit %d)\n", which, P::bottom.load(), P::beam_slots.load(), P::beam_slot.load());
    if (static_cast<P::Bottom>(P::bottom.load()) != P::Bottom::Hud) return;
    const auto now = Clock::now();
    if (which == 0) {
        tap_x = 130;   // the middle of the map (x 20-240, y 50-205)
        tap_y = 127;
    } else {
        const int present = P::beam_slots.load() | 1;   // the usual beam's slot is always there
        if (present == 1) return;
        int current = now < beam_settled ? beam_target : P::beam_slot.load();
        if (current < 0) current = 0;
        const int slots = static_cast<int>(BC::BeamSlotRects.size()), step = which == 2 ? slots - 1 : 1;   // 2: the one before
        int next = current;
        do {
            next = (next + step) % slots;
        } while ((present & (1 << next)) == 0);
        const auto& [x0, y0, x1, y1] = BC::BeamSlotRects[next];
        tap_x = (x0 + x1) / 2;
        tap_y = (y0 + y1) / 2;
        beam_target = next;
        beam_settled = now + std::chrono::milliseconds(500);
    }
    tap_until = now + std::chrono::milliseconds(100);
}

extern "C" int msr_injected_touch(int* x, int* y) {
    // touch navigation's finger, held or just lifted (shared clock: steady nanoseconds, as touch_nav.cpp writes)
    const long long now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    if (P::touch_hold.load(std::memory_order_relaxed) || now_ns < P::touch_tap_until.load(std::memory_order_relaxed)) {
        *x = P::tap_x.load();
        *y = P::tap_y.load();
        return 1;
    }
    if (Clock::now() >= tap_until) return 0;
    *x = tap_x;
    *y = tap_y;
    return 1;
}

// ---- touch navigation (src/msr/touch_nav.cpp, as the emulator build's input_bridge.cpp drives it): Tab, or L3 while
// a menu shows, puts a highlight on the touch screen's buttons (or a free cursor) that the D-pad, arrows or left
// stick move; the right face button, Return or Space is the finger (hold to drag); the bottom face button or Escape
// leaves. The game gets no other input meanwhile, nor until the button that closed it is released. ----
namespace {
MSR::TouchNavigator touch;
int stick_dx = 0, stick_dy = 0;
Clock::time_point stick_repeat{};
Clock::time_point last_touch_frame{};
} // namespace

extern "C" int msr_touch_active(void) { return touch.Active(); }
extern "C" void msr_touch_toggle(void) { touch.SetActive(!touch.Active()); }
// L3: only while a menu is on screen, or to leave
extern "C" void msr_touch_l3(void) { if (touch.Active() || P::panel_visible.load()) msr_touch_toggle(); }
extern "C" void msr_touch_step(int dx, int dy) { if (touch.Active()) touch.Step(dx, dy); }
extern "C" void msr_touch_finger(int down) { if (!touch.Active()) return; if (down) touch.Press(); else touch.Release(); }
extern "C" void msr_touch_close(void) { if (touch.Active()) touch.SetActive(false); }

// Each frame: the screen's buttons followed, the stick stepping between them (repeating while held) or moving the
// free point; and the game's input given back once nothing is held after touch navigation closed.
extern "C" void msr_touch_frame(float x, float y, int any_held) {
    const auto now = Clock::now();
    const float seconds = last_touch_frame.time_since_epoch().count() ? std::min(.1f, std::chrono::duration<float>(now - last_touch_frame).count()) : 0.f;
    last_touch_frame = now;
    if (!touch.Active()) {
        // the settings overlay keeps the game's input while it shows (overlay_host.cpp)
        if (P::block_game_input.load() && !any_held && !msr_overlay_visible()) P::block_game_input = false;
        stick_dx = stick_dy = 0;
        return;
    }
    touch.Update();
    if (touch.Snapping()) {
        const int sx = x > .5f ? 1 : x < -.5f ? -1 : 0, sy = y > .5f ? -1 : y < -.5f ? 1 : 0;
        if (sx != stick_dx || sy != stick_dy) {
            stick_dx = sx;
            stick_dy = sy;
            if (sx || sy) {
                touch.Step(sx, sy);
                stick_repeat = now + std::chrono::milliseconds(350);
            }
        } else if ((sx || sy) && now >= stick_repeat) {
            touch.Step(sx, sy);
            stick_repeat = now + std::chrono::milliseconds(140);
        }
    } else {
        touch.Glide(x, y, seconds);
    }
}
extern "C" int msr_input_blocked(void) { return P::block_game_input.load(std::memory_order_relaxed); }

