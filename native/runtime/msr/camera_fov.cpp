// The camera zoom without objects popping in at the edges (msr-native). The zoom (and the 16:9 view, which shows 1/15
// more of the level at the sides) scales the top screen's draws in clip space (pica_vk.c, msr_world_scale): the picture
// is right, but the game culls and switches objects on by its own field of view, so things beyond the 3DS's frame
// appeared only as they crossed it. Here the game's camera draws with a field of view wide enough for what the zoomed
// view shows, and the renderer scales every draw made with it back by the same factor: the picture is the same, and the
// game draws everything in it.
//
// The camera (Samus Returns USA): the one object with the vtable 0x6D1FD4; +0x4C is the horizontal field of view in
// degrees (45 in play, 28 in a save station; it eases to +0x70, which the camera logic sets). Each frame its manager runs
// the camera's update (0x33715C, called at 0x2EB644: logic, easing, and keeping the view inside the room's camera limits),
// then copies the camera's position and field of view and hands them to the scene's render camera (0x173A70, called at
// 0x2EB850, the field of view in s0), which keeps the field of view and what comes of it (the half-angle tangents, the
// culling cone's sine and cosine: RenderFov) and builds the projection. That call's s0 is widened on entry (a call hook,
// dispatch.cpp), so the scene is drawn and culled wider. The camera's limits are kept with the render camera's last
// field of view: for the update, the render camera gets the game's own values back (and the next set-up widens them
// again), so the camera stays where it is at zoom 1 (with the wider values it moved off that place near the room's
// edges and when the camera changed areas: the user saw the focus drift).
// A field of view whose tangent is G times wider brings every projected point G times closer to the screen's centre,
// so clip x and y times G give the same picture (msr_fov_gain, measured per draw from its projection).
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include "msr.h"
#include "common/common_types.h"
#include "msr/presentation.h"
#include "cpu.h"
extern "C" {
#include "kernel.h"
}
extern "C" unsigned g_gsp_top_swaps;   /* gsp.c: game frames */

namespace {

namespace P = MSR::Presentation;

constexpr u32 Vtable = 0x6D1FD4;
constexpr std::array<u32, 3> VtableWords{0x337450, 0x5B1770, 0x33629C};   // its words 1-3 (functions): this game, this version
constexpr u32 UpdateFn = 0x33715C, UpdateReturn = 0x2EB648;     // the camera's update, called by its manager
constexpr u32 SetViewFn = 0x173A70, SetViewReturn = 0x2EB854;   // the render camera's set-up, called by the manager
constexpr u32 SetViewCamera = 0xD8;                             // in the set-up's r0: the render camera
constexpr u32 FovNow = 0x4C, FovTarget = 0x70, Size = 0x100;
constexpr float Rad = 3.14159265f / 180.f;
constexpr u32 GamePointer = 0x76F314;                           // the game; Samus's entity through a handle (msr/cheats.h)
constexpr std::array<u32, 3> PlayerHandles{0xFC, 0xF0, 0xF4};

bool Valid(u32 address) { return address >= 0x00100000 && k_region_state(address) != 0; }
float Rf(u32 a) { u32 w = rd32(a); float f; std::memcpy(&f, &w, 4); return f; }
void Wf(u32 a, float f) { u32 w; std::memcpy(&w, &f, 4); wr32(a, w); }
bool Sane(float fov) { return fov > 1.f && fov < 170.f; }
// a field of view whose half-angle tangent is g times the given one's
float Widen(float fov, float g) { return std::fmin(2.f * std::atan(g * std::tan(fov * Rad * .5f)) / Rad, 170.f); }

struct {
    std::atomic<int> scan{0};      // 0 idle, 1 running, 2 done
    std::atomic<u32> found{0};
    std::atomic<u32> camera{0};    // set by the main thread, read by the hooks (game thread)
    double next_scan = 0;
    std::atomic<float> gain{1};    // wanted (eased), main thread -> hooks
    float eased = 1;
    double last_s = 0;
    int supported = -1;
    std::array<std::atomic<uint64_t>, 8> history{};   // (frame << 32 | gain's bits), 0 = none; read by the GX thread
    unsigned hist_n = 0;
    std::atomic<float> published_now{0};   // the game's own current field of view, for the renderer
    // game thread (hooks): the render camera and the fields of view last handed to it, the game's and the widened one
    u32 render = 0;
    float render_own = 0, render_wide = 0;
} cam;

u32 FindCamera() {
    uint32_t base[64], size[64], state[64];
    const int n = k_region_list(base, size, state, 64);
    for (int r = 0; r < n; ++r) {
        if ((state[r] != 5 && state[r] != 7) || base[r] < 0x08000000 || size[r] < Size) continue;
        for (u32 o = 0; o + Size <= size[r]; o += 4) {
            const u32 at = base[r] + o;
            if (rd32(at) == Vtable && Sane(Rf(at + FovNow)) && Sane(Rf(at + FovTarget))) return at;
        }
    }
    return 0;
}

void Note(float gain) {
    u32 bits; std::memcpy(&bits, &gain, 4);
    cam.history[cam.hist_n++ % cam.history.size()].store(static_cast<uint64_t>(g_gsp_top_swaps) << 32 | bits, std::memory_order_release);
}

// The render camera's fields that come from its field of view f (as 0x19997C sets them): the half-angle tangents
// (+0x4C, and +0x48 over its aspect +0x64), f (+0x50), the vertical angle (+0x54, radians), and the culling cone (half
// the angle plus a degree: sine, cosine, 1 / cosine, cosine squared, tangent at +0x1D8..+0x1E8).
void RenderFov(u32 rc, float f) {
    const float aspect = Rf(rc + 0x64);
    if (!(aspect > .1f && aspect < 10.f)) return;
    const float t = std::tan(f * Rad * .5f), tv = t / aspect, m = (f * .5f + 1.f) * Rad, sn = std::sin(m), cs = std::cos(m);
    Wf(rc + 0x48, tv);
    Wf(rc + 0x4C, t);
    Wf(rc + 0x50, f);
    Wf(rc + 0x54, 2.f * std::atan(tv));
    Wf(rc + 0x1D8, sn);
    Wf(rc + 0x1DC, cs);
    Wf(rc + 0x1E0, 1.f / cs);
    Wf(rc + 0x1E4, cs * cs);
    Wf(rc + 0x1E8, sn / cs);
}

// On entry to the camera's update from its manager: the render camera's fields as the game set them (where they are
// still the widened ones the last set-up made), for the camera's limits.
void UpdateHook(CPU* c) {
    if (c->r[14] != UpdateReturn) return;
    const u32 at = cam.camera.load(std::memory_order_relaxed), rc = cam.render;
    if (!at || c->r[0] != at || !rc || cam.render_wide == cam.render_own || !Valid(rc + 0x1EC)) return;
    if (std::fabs(Rf(rc + 0x50) - cam.render_wide) > 1e-3f) return;   // set since by someone else
    RenderFov(rc, cam.render_own);
}

// On entry to the render camera's set-up from the camera's manager: the field of view (s0), widened by the zoom's gain.
void SetViewHook(CPU* c) {
    if (c->r[14] != SetViewReturn) return;
    const u32 at = cam.camera.load(std::memory_order_relaxed);
    float own; std::memcpy(&own, &c->s[0], 4);
    const float g = cam.gain.load(std::memory_order_relaxed);
    if (!at || !Sane(own) || std::fabs(own - Rf(at + FovNow)) > .01f) return;   // the camera's own, copied
    cam.published_now.store(own, std::memory_order_relaxed);
    Note(g);
    const u32 rc = rd32(c->r[0] + SetViewCamera);
    const float wide = g > 1.f ? Widen(own, g) : own;
    cam.render = Valid(rc + 0x1EC) ? rc : 0;
    cam.render_own = own;
    cam.render_wide = wide;
    if (wide != own) std::memcpy(&c->s[0], &wide, 4);
}

} // namespace

// Each VBlank (main.c), on the main thread: find the camera, choose the gain the zoom asks for.
extern "C" void msr_camera_frame(double now) {
    if (cam.supported < 0) {
        cam.supported = 1;
        for (std::size_t i = 0; i < VtableWords.size(); ++i)
            if (rd32(Vtable + 4 + 4 * static_cast<u32>(i)) != VtableWords[i]) cam.supported = 0;
        if (cam.supported && getenv("PWR_NO_CAMERA_FOV")) cam.supported = 0;
        // push {r4-r6, lr} at both; bl 0x173A70 before its return address
        if (cam.supported && (rd32(SetViewFn) != 0xE92D4070u || rd32(SetViewReturn - 4) != 0xEBFA2086u || rd32(UpdateFn) != 0xE92D4070u))
            cam.supported = 0;
        if (cam.supported && (!rt_add_call_hook(SetViewFn, SetViewHook) || !rt_add_call_hook(UpdateFn, UpdateHook))) cam.supported = 0;
        if (!cam.supported) std::fprintf(stderr, "msr: camera: not this game's version (or off), the zoom scales the picture only\n");
    }
    if (!cam.supported) return;
    const float seconds = cam.last_s > 0 ? static_cast<float>(std::fmin(now - cam.last_s, .1)) : 0.f;
    cam.last_s = now;

    // the camera: found again when its object is gone (an area load, a save state)
    u32 at = cam.camera.load();
    if (at && (!Valid(at + Size - 4) || rd32(at) != Vtable || !Sane(Rf(at + FovTarget)))) cam.camera.store(at = 0);
    if (!at) {
        if (cam.scan.load() == 2) {
            at = cam.found.exchange(0); cam.scan.store(0);
            if (at) {
                cam.camera.store(at);
                std::fprintf(stderr, "msr: camera at %08x (field of view %g)\n", at, Rf(at + FovTarget));
            }
        } else if (cam.scan.load() == 0 && now >= cam.next_scan && msr_world_in_play()) {
            cam.next_scan = now + 1.0;
            cam.scan.store(1);
            std::thread([] { cam.found.store(FindCamera()); cam.scan.store(2); }).detach();   // ~25 ms: not on this thread
        }
    }

    // the gain wanted: the zoomed view's widest axis over the game's, in play
    const float ws = P::widescreen.load(std::memory_order_relaxed) ? P::ViewScale() : 1.f;   // 15/16 at 16:9, 15/32 at 32:9
    const float zoom = P::world_zoom.load(std::memory_order_relaxed);
    const float want = at && msr_world_in_play() ? std::fmax(1.f, 1.f / (ws * zoom)) : 1.f;
    cam.eased += (want - cam.eased) * (1.f - std::exp(-8.f * seconds));
    if (std::fabs(want - cam.eased) < .0005f) cam.eased = want;
    cam.gain.store(cam.eased, std::memory_order_relaxed);
    if (!at) { cam.published_now.store(0.f, std::memory_order_relaxed); Note(1.f); }
}

// The renderer (pica_vk.c): the gain a draw made with the camera's projection (x scale px) was made with: the game's
// own current field of view's scale over the draw's, when that lies among the gains of the last frames (the projection
// lags the fields by a frame or two); otherwise (the game's field of view changing) the gain of two frames before.
extern "C" float msr_fov_gain(float px) {
    const unsigned frame = g_gsp_top_swaps;
    float g2 = 1.f, lo = 4.f, hi = 1.f; unsigned best = 0; bool any = false;
    for (const auto& h : cam.history) {
        const uint64_t v = h.load(std::memory_order_acquire);
        if (!v) continue;
        const unsigned f = static_cast<unsigned>(v >> 32);
        float g; const u32 bits = static_cast<u32>(v); std::memcpy(&g, &bits, 4);
        if (f + 4 >= frame && f <= frame) { lo = std::fmin(lo, g); hi = std::fmax(hi, g); }
        if (f + 2 > frame || (any && f < best)) continue;
        g2 = g; best = f; any = true;
    }
    if (!any || !(g2 >= 1.f && g2 < 4.f)) g2 = 1.f;
    const float t = cam.published_now.load(std::memory_order_relaxed);
    if (Sane(t) && px > 0.f && hi >= lo) {
        const float r = (1.f / std::tan(t * Rad * .5f)) / px;
        if (r >= lo - 1e-4f && r <= hi + 1e-4f) return r;
    }
    return g2;
}

// video.c (mouse aiming): Samus on the top screen, in the game's own view (zoom 1, the 3DS's 5:3 frame): *h to the right
// and *v up, -1..1 across it. The point is her entity's position (+0xFC, also its transform's translation at +0x70 and
// +0x80), which the camera (looking straight down -z) shows at about her arm cannon's height. 0 outside play.
extern "C" int msr_samus_view(float* h, float* v) {
    const u32 at = cam.camera.load(std::memory_order_relaxed);
    if (!at || !msr_world_in_play() || !Valid(at + Size - 4) || rd32(at) != Vtable) return 0;
    const u32 game = rd32(GamePointer);
    for (const u32 offset : PlayerHandles) {
        if (!Valid(game + offset + 4)) return 0;
        const u32 handle = rd32(game + offset), entity = Valid(handle + 12) ? rd32(handle + 8) : 0;
        if (!Valid(entity + 0x108)) continue;
        const float x = Rf(entity + 0xFC), y = Rf(entity + 0x100), z = Rf(entity + 0x104);
        if (Rf(entity + 0x70) != x || Rf(entity + 0x80) != y) continue;
        const float dz = Rf(at + 0x3C) - z, t = std::tan(Rf(at + FovNow) * Rad * .5f);
        if (!(dz > 1.f) || !(t > 0.f)) return 0;
        *h = (x - Rf(at + 0x34)) / (dz * t);
        *v = (y - Rf(at + 0x38)) / (dz * t * (240.f / 400.f));
        return std::isfinite(*h) && std::isfinite(*v) && std::fabs(*h) < 4.f && std::fabs(*v) < 4.f;
    }
    return 0;
}
