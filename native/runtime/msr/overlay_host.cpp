// The emulator-based build's settings overlay on msr-native: src/msr/overlay.cpp itself (Dear ImGui), so the menu,
// its tabs and its look are the same. video.c hands it SDL's events and draws it at the end of its presentation pass;
// its actions are applied here: display and sound through video.c, main.c and dsp.c, the controls through input.c's
// bindings (saved in settings.ini [native_controls]), the mods and texture packs through mods.cpp, as the launcher
// does. Display and sound changes are also written to settings.ini [native] for the launcher to take over.
#include <SDL.h>
#include <imgui.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include "ini.h"
#include "mods.h"
#include "msr.h"
#include "msr/frame_rate.h"
#include "msr/overlay.h"
#include "msr/presentation.h"

extern "C" {
const char* input_control_label(int c);
const char* input_control_key(int c);
int* input_key_binds(void);
int* input_pad_binds(void);
void input_default_keys(void);
void input_default_pad(void);
void input_binding_text(int c, char* buf, size_t n);
void input_bind_key(int c, int sym);
void input_bind_pad(int c, int button);
void input_release_all(void);
void input_set_right_stick_dpad(int on);
int input_right_stick_dpad(void);
void video_set_fullscreen(int on);
void video_set_vsync(int on);
void video_set_resolution(int factor);
int video_pad_connected(void);
int video_pad_any_held(void);
void dsp_set_volume(float volume);
void main_set_frame_rate(int fps);
void main_exit(int code);
void msr_launcher_action(int kind, const char* text);
extern unsigned g_gsp_top_swaps;
}

namespace {

constexpr int Controls = 18;   // input.c's C_COUNT: A ... D-pad right, Circle Pad up ... right
constexpr int Home = 99, CirclePad = 100, CStick = 101;

MSR::Overlay& O() {
    return MSR::GetOverlay();
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

MSR::EmulatorSettings emulator, launched;   // now, and as the launcher started msr-native
bool launcher_mode = false;                 // the program is the launcher (launcher.cpp): no game behind the menu
std::mutex upload_lock;                     // video.c holds its queue lock around Init
int capturing = -1;                         // the control waiting for a new key or button
bool capture_armed = false;                 // the pad button that chose it has been released
double capture_until = 0;

void RefreshControls() {
    MSR::ControlsSnapshot s;
    s.profile = "msr-native";
    char text[128];
    for (int c = 0; c < Controls; ++c) {
        input_binding_text(c, text, sizeof text);
        s.rows.push_back({c, input_control_label(c), text});
    }
    s.rows.push_back({Home, "Home", "Not set"});
    s.rows.push_back({CirclePad, "Circle Pad", "Gamepad left stick (and the arrow keys)"});
    s.rows.push_back({CStick, "C-Stick", "Not set"});
    s.capturing = capturing;
    s.controller_connected = video_pad_connected() != 0;
    s.right_stick_option = true;
    s.right_stick_dpad = input_right_stick_dpad() != 0;
    s.menu_layout_option = true;
    s.menu_confirm_right = msr_menu_confirm_right() != 0;
    O().SetControls(s);
}

void SaveControls() {
    MsrIni::File file(msr_settings_file());
    for (int c = 0; c < Controls; ++c) {
        const std::string key = input_control_key(c);
        file.SetNumber("native_controls", key + "_key", input_key_binds()[c]);
        file.SetNumber("native_controls", key + "_pad", input_pad_binds()[c]);
    }
    file.SetBool("native_controls", "right_stick_dpad", input_right_stick_dpad() != 0);
    file.SetBool("native_controls", "menu_confirm_right", msr_menu_confirm_right() != 0);
    if (!file.Save()) std::fprintf(stderr, "msr: could not save the controls\n");
}

void LoadControls() {
    const MsrIni::File file(msr_settings_file());
    input_set_right_stick_dpad(file.Bool("native_controls", "right_stick_dpad", input_right_stick_dpad() != 0));
    msr_set_menu_confirm_right(file.Bool("native_controls", "menu_confirm_right", msr_menu_confirm_right() != 0));
    for (int c = 0; c < Controls; ++c) {
        const std::string key = input_control_key(c);
        if (file.Raw("native_controls", key + "_key")) {
            input_key_binds()[c] = static_cast<int>(file.Number("native_controls", key + "_key", input_key_binds()[c]));
        }
        if (file.Raw("native_controls", key + "_pad")) {
            input_pad_binds()[c] = static_cast<int>(file.Number("native_controls", key + "_pad", input_pad_binds()[c]));
        }
    }
}

/// The Mods and Textures tabs' lists, from the folders and settings.ini. Any thread: it only reads.
MSR::ModsSnapshot ScanMods() {
    MSR::ModsSnapshot s;
    for (const auto& mod : NativeMods::GameMods()) s.mods.push_back({mod.name, NativeMods::Describe(mod), mod.enabled});
    s.mod_folder = NativeMods::GameModFolder();
    for (const auto& pack : NativeMods::TexturePacks()) s.packs.push_back({pack.name, NativeMods::Describe(pack), pack.enabled});
    s.folder = NativeMods::TexturePackFolder();
    s.preload = true;   // msr-native reads every pack's file list at start and loads the images as they are used
    s.dump = false;
    s.texture_options = false;
    s.restart_needed = !launcher_mode && NativeMods::Pending() != NativeMods::Booted();
    return s;
}

std::atomic<unsigned> mods_generation{0};   // raised by each RefreshMods: a background scan begun before it is stale

void RefreshMods() {
    ++mods_generation;
    O().SetMods(ScanMods());
}

/// While the Mods or Textures tab shows, their folders are scanned again about once a second (native/README.md), so a
/// pack or mod copied in with the menu open appears. The scan walks every file of every pack (thousands for a large
/// one): a thread of its own. A scan begun before a change in the menu is dropped, so the change never flicks back.
/// Allocated once and never freed: its thread is joined in msr_overlay_shutdown, and no static destructor touches
/// its condition variable (the launcher leaves through exit()).
struct ModsScanner {
    std::mutex lock;
    std::condition_variable wake;
    std::thread thread;
    bool wanted = false, quit = false, ready = false;
    unsigned wanted_generation = 0, generation = 0;   // mods_generation when the waiting scan / the result was asked
    MSR::ModsSnapshot result;
    double next = 0;   // render thread: when the next scan is due

    void Run() {
        std::unique_lock guard{lock};
        for (;;) {
            wake.wait(guard, [this] { return wanted || quit; });
            if (quit) return;
            wanted = false;
            const unsigned asked = wanted_generation;
            guard.unlock();
            MSR::ModsSnapshot found = ScanMods();
            guard.lock();
            result = std::move(found);
            generation = asked;
            ready = true;
        }
    }
};

ModsScanner& Scanner() {
    static ModsScanner* scanner = new ModsScanner;
    return *scanner;
}

/// Each frame, after the menu's actions: a finished scan shown, and the next one asked for while the tabs show.
void PollModsScan() {
    ModsScanner& sc = Scanner();
    MSR::ModsSnapshot found;
    bool fresh = false;
    {
        std::scoped_lock guard{sc.lock};
        if (sc.ready) {
            sc.ready = false;
            fresh = sc.generation == mods_generation;
            if (fresh) found = std::move(sc.result);
        }
    }
    if (fresh) O().SetMods(found);
    if (!O().ModsTabShown()) {
        sc.next = 0;   // scanned at once when the tab shows again
        return;
    }
    const double now = Now();
    if (now < sc.next) return;
    sc.next = now + 1.0;
    std::scoped_lock guard{sc.lock};
    if (sc.quit) return;
    if (!sc.thread.joinable()) sc.thread = std::thread([&sc] { sc.Run(); });
    sc.wanted = true;
    sc.wanted_generation = mods_generation;
    sc.wake.notify_one();
}

void StopModsScan() {
    ModsScanner& sc = Scanner();
    {
        std::scoped_lock guard{sc.lock};
        sc.quit = true;
    }
    sc.wake.notify_one();
    if (sc.thread.joinable()) sc.thread.join();
}

/// What the overlay changed, applied, and kept for the launcher where it differs from what the launcher gave. In the
/// launcher they are only kept (they are the game's: its window stays as it is) and written whatever they are.
void ApplyEmulator(const MSR::EmulatorSettings& e) {
    if (launcher_mode) {
        emulator = e;
        MsrIni::File file(msr_settings_file());
        file.SetBool("native", "fullscreen", e.fullscreen);
        file.SetNumber("native", "resolution", e.resolution);
        file.SetBool("native", "vsync", e.vsync);
        file.SetNumber("native", "volume", e.volume);
        if (!file.Save()) std::fprintf(stderr, "msr: could not save the display settings\n");
        return;
    }
    if (e.fullscreen != emulator.fullscreen) video_set_fullscreen(e.fullscreen);
    if (e.resolution != emulator.resolution) video_set_resolution(e.resolution);
    if (e.vsync != emulator.vsync) video_set_vsync(e.vsync);
    if (e.volume != emulator.volume) dsp_set_volume(static_cast<float>(e.volume) / 100.f);
    emulator = e;
    MsrIni::File file(msr_settings_file());
    if (emulator.fullscreen != launched.fullscreen) file.SetBool("native", "fullscreen", emulator.fullscreen);
    if (emulator.resolution != launched.resolution) file.SetNumber("native", "resolution", emulator.resolution);
    if (emulator.vsync != launched.vsync) file.SetBool("native", "vsync", emulator.vsync);
    if (emulator.volume != launched.volume) file.SetNumber("native", "volume", emulator.volume);
    if (!file.Save()) std::fprintf(stderr, "msr: could not save the display settings\n");
}

ImGuiKey KeyOf(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return static_cast<ImGuiKey>(ImGuiKey_A + (k - SDLK_a));
    if (k >= SDLK_0 && k <= SDLK_9) return static_cast<ImGuiKey>(ImGuiKey_0 + (k - SDLK_0));
    if (k >= SDLK_F1 && k <= SDLK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (k - SDLK_F1));
    switch (k) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey PadKeyOf(int button) {
    // SDL names the face buttons by position (A = bottom; video.c asks it to for Nintendo pads too), as ImGui does.
    // overlay.cpp has ImGui choose with the right face button (ConfigNavSwapGamepadButtons, the game's layout): the
    // pad's choosing button (glyphs.cpp msr_pad_choose: the bottom one, the right one on a Nintendo pad) is given as it.
    // Its back button (msr_pad_back) is given as the bottom one, ImGui's back; the other face buttons choose and go
    // back nowhere (the Nintendo menu layout can put the game's A and B on any two).
    const int choose = msr_pad_choose(), back = msr_pad_back();
    if (button == choose) return ImGuiKey_GamepadFaceRight;
    if (button == back) return ImGuiKey_GamepadFaceDown;
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
    case SDL_CONTROLLER_BUTTON_B: return ImGuiKey_None;
    case SDL_CONTROLLER_BUTTON_X: return ImGuiKey_GamepadFaceLeft;
    case SDL_CONTROLLER_BUTTON_Y: return ImGuiKey_GamepadFaceUp;
    case SDL_CONTROLLER_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_CONTROLLER_BUTTON_BACK: return ImGuiKey_GamepadBack;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ImGuiKey_GamepadL1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ImGuiKey_GamepadR1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return ImGuiKey_GamepadL3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return ImGuiKey_GamepadR3;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    default: return ImGuiKey_None;
    }
}

void Modifiers(Uint16 mod) {
    O().Key(ImGuiMod_Ctrl, (mod & KMOD_CTRL) != 0);
    O().Key(ImGuiMod_Shift, (mod & KMOD_SHIFT) != 0);
    O().Key(ImGuiMod_Alt, (mod & KMOD_ALT) != 0);
}

} // namespace

extern "C" void msr_overlay_launcher(int on) {
    launcher_mode = on != 0;
}

/// The launcher, after the game ended: what its menu changed (settings.ini).
extern "C" void msr_overlay_reload(void) {
    msr_settings_load(msr_settings_file());
    MsrIni::File file(msr_settings_file());
    emulator.fullscreen = file.Bool("native", "fullscreen", emulator.fullscreen);
    emulator.resolution = static_cast<int>(file.Number("native", "resolution", emulator.resolution));
    emulator.vsync = file.Bool("native", "vsync", emulator.vsync);
    emulator.volume = static_cast<int>(file.Number("native", "volume", emulator.volume));
    O().SetEmulatorSettings(emulator);
    LoadControls();
    RefreshControls();
    RefreshMods();
}

/// A Steam Deck (LCD: Jupiter, OLED: Galileo), by its firmware's names: the Display tab offers its preset
static bool SteamDeck() {
#ifdef __linux__
    const auto read = [](const char* path) {
        std::string s;
        if (FILE* f = std::fopen(path, "rb")) {
            char buf[64];
            const std::size_t n = std::fread(buf, 1, sizeof buf, f);
            std::fclose(f);
            s.assign(buf, n);
        }
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        return s;
    };
    const std::string vendor = read("/sys/devices/virtual/dmi/id/board_vendor"), product = read("/sys/devices/virtual/dmi/id/product_name");
    return vendor == "Valve" && (product == "Jupiter" || product == "Galileo");
#else
    return false;
#endif
}

extern "C" void msr_overlay_start(int fullscreen, int resolution, int vsync, int volume) {
    launched.fullscreen = fullscreen != 0;
    launched.resolution = resolution;
    launched.vsync = vsync != 0;
    launched.volume = volume;
    launched.max_resolution = 10;
    launched.quick_saves = true;
    launched.bloom_option = true;
    launched.ultrawide_option = true;
    launched.deck_preset = SteamDeck();
    emulator = launched;
    O().SetEmulatorSettings(emulator);
    LoadControls();
    RefreshControls();
    RefreshMods();
    if (!launcher_mode) O().ShowStartupHint();
}

extern "C" void msr_overlay_vk_init(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t family,
                                    VkQueue queue, VkFormat format, uint32_t images) {
    MSR::OverlayHook::VulkanContext c{};
    c.api_version = VK_API_VERSION_1_3;
    c.instance = instance;
    c.physical_device = physical;
    c.device = device;
    c.queue_family = family;
    c.queue = queue;
    c.render_pass = VK_NULL_HANDLE;
    c.image_count = images;
    c.get_instance_proc_addr = vkGetInstanceProcAddr;
    c.queue_mutex = &upload_lock;
    c.color_format = format;
    O().Init(c);
}

extern "C" void msr_overlay_draw(VkCommandBuffer command, uint32_t width, uint32_t height) {
    O().Draw(command, width, height);
}

extern "C" void msr_overlay_shutdown(void) {
    StopModsScan();
    O().Shutdown();
}

extern "C" int msr_overlay_visible(void) {
    return O().Visible();
}

extern "C" int msr_overlay_capturing(void) {
    return capturing >= 0;
}

extern "C" int msr_overlay_launcher_mode(void) {
    return launcher_mode;
}

extern "C" void msr_overlay_toggle(void) {
    if (!O().Visible()) {
        input_release_all();
        RefreshControls();   // a pad may have come or gone
    }
    O().Toggle();
}

/// video.c, once it knows the display: whether HDR can be turned on, the display's peak and Windows' SDR white (nits).
extern "C" void msr_overlay_hdr(int option, float peak, float paper) {
    launched.hdr_option = emulator.hdr_option = option;
    launched.display_peak = emulator.display_peak = peak;
    launched.display_paper = emulator.display_paper = paper;
    O().SetEmulatorSettings(emulator);
}

extern "C" void msr_overlay_fullscreen(int on) {   // F11 or Alt+Enter
    emulator.fullscreen = on != 0;
    O().SetEmulatorSettings(emulator);
}

/// The bottom screen's panel this frame (x, y, w, h in drawable pixels), or NULL when there is none: the control hints
/// go beside it.
extern "C" void msr_overlay_panel(const float* panel) {
    namespace P = MSR::Presentation;
    P::panel_x0 = panel ? static_cast<int>(panel[0]) : 0;
    P::panel_y0 = panel ? static_cast<int>(panel[1]) : 0;
    P::panel_x1 = panel ? static_cast<int>(panel[0] + panel[2]) : 0;
    P::panel_y1 = panel ? static_cast<int>(panel[1] + panel[3]) : 0;
}

/// An SDL event: 1 when the overlay took it (any input while it shows, or a key or button for a binding).
/// sx, sy: drawable pixels per window coordinate.
extern "C" int msr_overlay_event(const SDL_Event* e, float sx, float sy) {
    if (capturing >= 0) {
        if (e->type == SDL_KEYDOWN && !e->key.repeat) {
            if (e->key.keysym.sym != SDLK_ESCAPE) {
                input_bind_key(capturing, e->key.keysym.sym);
                SaveControls();
            }
            capturing = -1;
            RefreshControls();
            return 1;
        }
        if (e->type == SDL_CONTROLLERBUTTONDOWN && capture_armed) {
            input_bind_pad(capturing, e->cbutton.button);
            SaveControls();
            capturing = -1;
            RefreshControls();
            return 1;
        }
        if (e->type == SDL_KEYDOWN || e->type == SDL_KEYUP || e->type == SDL_CONTROLLERBUTTONDOWN ||
            e->type == SDL_CONTROLLERBUTTONUP || e->type == SDL_CONTROLLERAXISMOTION) {
            return 1;
        }
    }
    const bool visible = O().Visible();
    static const bool trace = std::getenv("PWR_INPUT_TRACE") != nullptr;
    if (trace && (e->type == SDL_MOUSEMOTION || e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP)) {
        std::fprintf(stderr, "overlay: mouse event %#x at %d,%d (visible %d)\n", e->type,
                     e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x,
                     e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y, visible);
    }
    switch (e->type) {
    case SDL_MOUSEMOTION:
        O().MousePosition(static_cast<float>(e->motion.x) * sx, static_cast<float>(e->motion.y) * sy);
        return visible;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (!visible) return 0;
        O().MousePosition(static_cast<float>(e->button.x) * sx, static_cast<float>(e->button.y) * sy);
        const int button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1
                         : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (button >= 0) O().MouseButton(button, e->type == SDL_MOUSEBUTTONDOWN);
        return 1;
    }
    case SDL_MOUSEWHEEL:
        if (!visible) return 0;
        O().MouseWheel(e->wheel.preciseY);
        return 1;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (!visible) return 0;
        if (e->key.repeat) return 1;   // ImGui repeats held keys itself
        Modifiers(e->key.keysym.mod);
        if (const ImGuiKey key = KeyOf(e->key.keysym.sym); key != ImGuiKey_None) O().Key(key, e->type == SDL_KEYDOWN);
        return 1;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (!visible) return 0;
        if (const ImGuiKey key = PadKeyOf(e->cbutton.button); key != ImGuiKey_None) {
            O().Key(key, e->type == SDL_CONTROLLERBUTTONDOWN);
        }
        return 1;
    case SDL_CONTROLLERAXISMOTION: {
        if (!visible) return 0;
        const float v = static_cast<float>(e->caxis.value) / 32767.f;
        const auto axis = [](ImGuiKey negative, ImGuiKey positive, float value) {
            const float dead = .2f;
            O().Analog(negative, value < -dead, value < -dead ? -value : 0.f);
            O().Analog(positive, value > dead, value > dead ? value : 0.f);
        };
        switch (e->caxis.axis) {
        case SDL_CONTROLLER_AXIS_LEFTX: axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, v); break;
        case SDL_CONTROLLER_AXIS_LEFTY: axis(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, v); break;
        case SDL_CONTROLLER_AXIS_RIGHTX: axis(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, v); break;
        case SDL_CONTROLLER_AXIS_RIGHTY: axis(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, v); break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: O().Analog(ImGuiKey_GamepadL2, v > .5f, v); break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: O().Analog(ImGuiKey_GamepadR2, v > .5f, v); break;
        }
        return 1;
    }
    default:
        return 0;
    }
}

/// Each presented frame, before drawing: the game's frame count (the FPS counter) and the overlay's actions.
extern "C" void msr_overlay_frame(void) {
    using Kind = MSR::OverlayAction::Kind;
    MSR::FrameRate::game_frames.store(g_gsp_top_swaps, std::memory_order_relaxed);
    if (capturing >= 0) {
        if (!capture_armed && !video_pad_any_held()) capture_armed = true;
        if (Now() > capture_until) {
            capturing = -1;
            RefreshControls();
        }
    }
    for (const auto& action : O().TakeActions()) {
        switch (action.kind) {
        case Kind::Emulator:
            ApplyEmulator(action.emulator);
            break;
        case Kind::Player:
            msr_settings_save();
            break;
        case Kind::Layout:
            break;   // the compositor reads the presentation settings each frame
        case Kind::FrameRate:
            main_set_frame_rate(O().GetPlayerSettings().frame_rate);
            break;
        case Kind::QuickSave:
            msr_state_quick_save();   // state.c
            break;
        case Kind::QuickLoad:
            msr_state_quick_load();
            break;
        case Kind::PreloadTextures:
        case Kind::DumpTextures:
            break;   // not on msr-native (the overlay shows them unavailable)
        case Kind::Restart:
            msr_settings_save();
            main_exit(MSR_EXIT_RESTART);
            break;
        case Kind::LauncherPlay:
        case Kind::LauncherBuild:
        case Kind::LauncherBuildAndroid:
        case Kind::LauncherCancel:
        case Kind::LauncherBrowse:
        case Kind::LauncherGame:
        case Kind::LauncherFolder:
        case Kind::LauncherUpdate:
        case Kind::LauncherUpdatePage:
            msr_launcher_action(static_cast<int>(action.kind), action.text.c_str());
            break;
        case Kind::Quit:
            if (launcher_mode) {
                msr_launcher_action(static_cast<int>(action.kind), nullptr);
                break;
            }
            msr_settings_save();
            main_exit(0);
            break;
        case Kind::Bind:
            if (action.value >= 0 && action.value < Controls) {
                capturing = action.value;
                capture_armed = false;
                capture_until = Now() + 8.0;
            }
            RefreshControls();
            break;
        case Kind::CancelBind:
            capturing = -1;
            RefreshControls();
            break;
        case Kind::GamepadLayout:
            input_default_pad();
            SaveControls();
            RefreshControls();
            break;
        case Kind::KeyboardLayout:
            input_default_keys();
            SaveControls();
            RefreshControls();
            break;
        case Kind::RightStickDpad:
            input_set_right_stick_dpad(action.value);
            SaveControls();
            RefreshControls();
            break;
        case Kind::MenuConfirmRight:
            msr_set_menu_confirm_right(action.value);
            SaveControls();
            RefreshControls();
            break;
        case Kind::GameMod:
        case Kind::GameModUp:
        case Kind::GameModDown: {
            const auto mods = NativeMods::GameMods();
            if (action.value >= 0 && action.value < static_cast<int>(mods.size())) {
                const auto& mod = mods[static_cast<std::size_t>(action.value)];
                if (action.kind == Kind::GameMod) NativeMods::SetModEnabled(mod.name, !mod.enabled);
                else NativeMods::MoveMod(mod.name, action.kind == Kind::GameModUp ? -1 : 1);
            }
            RefreshMods();
            break;
        }
        case Kind::GameModsFolder:
            NativeMods::OpenFolder(NativeMods::GameModFolder());
            break;
        case Kind::TexturePack:
        case Kind::TexturePackUp:
        case Kind::TexturePackDown: {
            const auto packs = NativeMods::TexturePacks();
            if (action.value >= 0 && action.value < static_cast<int>(packs.size())) {
                const auto& pack = packs[static_cast<std::size_t>(action.value)];
                if (action.kind == Kind::TexturePack) NativeMods::SetPackEnabled(pack.name, !pack.enabled);
                else NativeMods::MovePack(pack.name, action.kind == Kind::TexturePackUp ? -1 : 1);
            }
            RefreshMods();
            break;
        }
        case Kind::ModsFolder:
            NativeMods::OpenFolder(NativeMods::TexturePackFolder());
            break;
        }
    }
    PollModsScan();
}
