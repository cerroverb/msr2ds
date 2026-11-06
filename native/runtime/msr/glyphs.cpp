// The controller's buttons where the game and the port name them (msr-native): the player's device, from the last input
// (video.c), picks the game's face-button icons (src/msr/button_glyphs.h, drawn into the game's two icon textures as
// pica_vk.c uploads them, a texture pack's too) and the control reminders' names (overlay.cpp). An Xbox pad (also
// Steam's virtual pads and other pads SDL cannot name) shows the game's letter icons as Xbox labels its buttons, a
// PlayStation pad its symbols, a Nintendo pad the game's own icons; the keyboard the letter or digit of the key bound
// to each (the space bar's sign for Space, the game's icon where the key's name is longer) and in the L and R icons the
// letters of their keys (Q and E), and the reminders name its keys (the circle pad's and touch navigation's from the
// bindings too). MSR_BUTTON_ICONS=xbox|playstation|nintendo|keyboard forces one (tests).
// Unless the HUD shows (menus, the game's dialogs over play, cutscenes), an Xbox or PlayStation pad chooses with its
// bottom face button and goes back with its right one (input.c input_pad_bound, the menus' layout; compositor.cpp says
// when): the names follow. The icons follow it in menus (the bottom screen's and text's: the menus' check mark shows
// A or Cross there and B or Circle in play), not in the dialogs over play, whose text names play's buttons ("Press Ⓐ"
// for an Aeion ability); the dialogs' own "Ⓐ ✔" and "Ⓑ ✖" (the top screen's atlas, only used there) always show it.
// The Controls tab's Nintendo menu layout (2026-11-03, msr2ds issue 1: "whatever you mapped A to is presented as A
// both in-game and on the overlay, and has the typical function of the A button"; off by default) turns the menus'
// layout off for every pad: menus, dialogs, touch mode and the settings choose with the button bound to the game's A
// and go back with the one bound to its B, and the icons and names say so.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <SDL.h>
#include "msr.h"
#include "common/cityhash.h"
#include "msr/button_glyphs.h"
extern "C" {
#include "input.h"
}

namespace {

namespace G = MSR::ButtonGlyphs;

std::atomic<unsigned> version{1};
u32 device_type = SDL_CONTROLLER_TYPE_UNKNOWN;   // the pad's, when the device is a pad
// compositor.cpp (msr_buttons_state): the pad's buttons follow the menus' layout (anything but the HUD), the icons do
// (a menu shows); the game starts in its menus
std::atomic<bool> menu_input{true}, menu_icons{true};
// the Controls tab's Nintendo menu layout (overlay_host.cpp, settings.ini [native_controls] menu_confirm_right)
std::atomic<bool> confirm_right{false};
// the top screen's atlas (Sheets[2], the dialogs' A and B): its faces always in the menus' layout
std::atomic<u32> dialog_shown{G::Identity}, dialog_symbols{0};
constexpr int DialogSheet = 3;

u32 DeviceOf(int sdl_type) {
    switch (sdl_type) {
    case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4: case SDL_CONTROLLER_TYPE_PS5: return G::PlayStation;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return G::Nintendo;
    default: return G::Xbox;   // Xbox, Steam's virtual pads (SDL names the pad behind them when it can), others
    }
}

u32 Forced() {
    static int forced = -1;
    if (forced < 0) {
        const char* e = std::getenv("MSR_BUTTON_ICONS");
        forced = !e ? 0 : !std::strcmp(e, "xbox") ? G::Xbox : !std::strcmp(e, "playstation") || !std::strcmp(e, "ps") ? G::PlayStation
                 : !std::strcmp(e, "nintendo") ? G::Nintendo : !std::strcmp(e, "keyboard") ? G::Keyboard : 0;
    }
    return static_cast<u32>(forced);
}

// the family whose layout the pad's buttons follow: the forced icons' (tests), else the pad used last
u32 PadFamily() {
    const u32 f = Forced();
    return f == G::Xbox || f == G::PlayStation || f == G::Nintendo ? f : DeviceOf(static_cast<int>(device_type));
}
// a pad of this family takes the menus' layout (not a Nintendo pad, whose labels already are the game's layout; not
// with the Nintendo menu layout), and now: its buttons (input), its icons
bool MenusLayoutFor(u32 family) { return family != G::Nintendo && !confirm_right.load(std::memory_order_relaxed); }
bool MenuLayout(u32 family, bool icons = false) {
    return MenusLayoutFor(family) && (icons ? menu_icons : menu_input).load(std::memory_order_relaxed);
}
// the buttons that choose and go back in menus, touch mode and the settings: the bottom and right ones (a Nintendo
// pad's right and bottom ones); with the Nintendo menu layout, the face buttons bound to the game's A and B
bool Face(int b) { return b >= SDL_CONTROLLER_BUTTON_A && b <= SDL_CONTROLLER_BUTTON_Y; }
int ChooseButton(u32 family) {
    if (confirm_right.load(std::memory_order_relaxed)) {
        const int a = input_pad_binds()[C_A];
        return Face(a) ? a : SDL_CONTROLLER_BUTTON_B;
    }
    return family == G::Nintendo ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
}
int BackButton(u32 family) {
    const int choose = ChooseButton(family);
    if (confirm_right.load(std::memory_order_relaxed)) {
        const int b = input_pad_binds()[C_B];
        if (Face(b) && b != choose) return b;
    }
    return choose == SDL_CONTROLLER_BUTTON_A ? SDL_CONTROLLER_BUTTON_B : SDL_CONTROLLER_BUTTON_A;
}

// a pad button's label on the device in use (SDL maps buttons by position: A south, B east, X west, Y north)
const char* Label(int button) {
    const u32 dev = G::device.load(std::memory_order_relaxed);
    static const char* const xbox[] = {"A", "B", "X", "Y", "View", "Guide", "Menu", "LS", "RS", "LB", "RB", "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
    static const char* const ps[] = {"Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
    static const char* const nin[] = {"B", "A", "Y", "X", "-", "Home", "+", "LS", "RS", "L", "R", "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
    if (button == PAD_TRIGGER_LEFT || button == PAD_TRIGGER_RIGHT) {   // the triggers as buttons (input.h, #35)
        static const char* const triggers[3][2] = {{"LT", "RT"}, {"L2", "R2"}, {"ZL", "ZR"}};
        return triggers[dev == G::PlayStation ? 1 : dev == G::Nintendo ? 2 : 0][button == PAD_TRIGGER_RIGHT];
    }
    if (button < 0 || button > SDL_CONTROLLER_BUTTON_DPAD_RIGHT) return "";
    return dev == G::PlayStation ? ps[button] : dev == G::Nintendo ? nin[button] : xbox[button];
}

// the pad button a control is bound to, by the device's name for it ("" for none): ZL and ZR's are the triggers by
// default, any button or trigger once rebound (#35)
const char* PadName(u32 control) {
    const u32 dev = G::device.load(std::memory_order_relaxed);
    const int b = control < C_COUNT ? input_pad_bound(static_cast<int>(control), MenuLayout(dev)) : -1;
    return b >= 0 ? Label(b) : "";
}

const char* RoleName(u32 role) {
    const u32 dev = G::device.load(std::memory_order_relaxed);
    switch (role) {
    case G::PadChoose: return Label(ChooseButton(dev));
    case G::PadCancel: return Label(BackButton(dev));
    case G::PadSouth: return Label(SDL_CONTROLLER_BUTTON_A);
    case G::PadEast: return Label(SDL_CONTROLLER_BUTTON_B);
    case G::PadWest: return Label(SDL_CONTROLLER_BUTTON_X);
    case G::PadNorth: return Label(SDL_CONTROLLER_BUTTON_Y);
    case G::PadL1: return Label(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    case G::PadR1: return Label(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    case G::PadL3: return Label(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    case G::PadBack: return Label(SDL_CONTROLLER_BUTTON_BACK);
    case G::PadStick: return "Left stick";
    case G::PadDpad: return "D-pad";
    default: return "";
    }
}

std::string BoundKey(int control) {
    const int sym = control >= 0 && control < C_COUNT ? input_key_binds()[control] : -1;
    if (sym < 0) return "";
    std::string name = SDL_GetKeyName(static_cast<SDL_Keycode>(sym));
    if (name == "Return") name = "Enter";
    else if (name == "Escape") name = "Esc";
    return name;
}

// a control's key, or one of the keyboard's own roles (G::KeyStick ...: the circle pad's keys, touch navigation's)
const char* KeyName(u32 control) {
    static thread_local std::string name;
    const auto either = [](const std::string& a, const char* b) { return a.empty() ? std::string(b) : a + " / " + b; };
    switch (control) {
    case G::KeyStick: case G::KeyMove: {
        // the circle pad's four keys: "WASD" when each is a letter, else their names
        const std::string up = BoundKey(C_CUP), left = BoundKey(C_CLEFT), down = BoundKey(C_CDOWN), right = BoundKey(C_CRIGHT);
        if (up.size() == 1 && left.size() == 1 && down.size() == 1 && right.size() == 1) name = up + left + down + right;
        else if (!up.empty() || !left.empty() || !down.empty() || !right.empty()) name = up + " " + left + " " + down + " " + right;
        else name = "";
        name = control == G::KeyMove || name.empty() ? either(name, "arrows") : name;
        break;
    }
    case G::KeyTouch: name = "T"; break;
    case G::KeyTap: name = either(BoundKey(C_A), "Enter"); break;
    case G::KeyLeave: name = either(BoundKey(C_B), "Esc"); break;
    default: name = BoundKey(control < C_COUNT ? static_cast<int>(control) : -1); break;
    }
    return name.c_str();
}

// the icons and symbols for the device and the bindings: each face shows the button it is bound to (in menus, the
// menus' layout; the dialogs' sheet always)
void Apply(u32 dev) {
    u32 shown = G::Identity, symbols = 0, letters = 0, shoulders = 0, dshown = G::Identity, dsymbols = 0;
    if (dev == G::Xbox || dev == G::Nintendo || dev == G::PlayStation) {
        static const u32 nintendo_letter[4] = {G::B, G::A, G::Y, G::X};   // south, east, west, north
        static const u32 ps_symbol[4] = {G::Cross, G::Circle, G::Square, G::Triangle};
        const auto faces = [&](bool menu, u32& out_shown, u32& out_symbols) {
            for (u32 face = 0; face < G::FaceCount; ++face) {
                const int b = input_pad_bound(static_cast<int>(face), menu);
                if (b < SDL_CONTROLLER_BUTTON_A || b > SDL_CONTROLLER_BUTTON_Y) continue;   // not a face button: its own icon
                const u32 show = dev == G::Xbox ? static_cast<u32>(b) : dev == G::Nintendo ? nintendo_letter[b] : face;
                out_shown = (out_shown & ~(3u << (face * 2))) | show << (face * 2);
                if (dev == G::PlayStation) out_symbols |= ps_symbol[b] << (face * 3);
            }
        };
        faces(MenuLayout(dev, true), shown, symbols);
        faces(MenusLayoutFor(dev), dshown, dsymbols);
    }
    if (dev == G::Keyboard) {
        for (u32 face = 0; face < G::FaceCount; ++face) {
            const int sym = input_key_binds()[face];
            const char* name = sym >= 0 ? SDL_GetKeyName(static_cast<SDL_Keycode>(sym)) : "";
            if (name && name[0] && !name[1] && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
                letters |= static_cast<u32>(name[0]) << (face * 8);
            else if (sym == SDLK_SPACE)
                letters |= static_cast<u32>(' ') << (face * 8);   // the space bar's sign
        }
        for (u32 i = 0; i < G::ShoulderCount; ++i) {   // the L and R icons: the keys L and R are bound to (Q and E)
            const int sym = input_key_binds()[C_L + static_cast<int>(i)];
            const char* name = sym >= 0 ? SDL_GetKeyName(static_cast<SDL_Keycode>(sym)) : "";
            if (name && name[0] && !name[1] && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
                shoulders |= static_cast<u32>(name[0]) << (i * 8);
        }
    }
    const bool device = G::device.exchange(dev) != dev;
    const bool changed = device | G::shown.exchange(shown) != shown | G::symbols.exchange(symbols) != symbols |
                         G::letters.exchange(letters) != letters | G::shoulder_letters.exchange(shoulders) != shoulders |
                         dialog_shown.exchange(dshown) != dshown | dialog_symbols.exchange(dsymbols) != dsymbols;
    if (changed) version.fetch_add(1);
    if (device) {
        static const char* const names[] = {"unknown", "keyboard", "Xbox", "PlayStation", "Nintendo"};
        std::fprintf(stderr, "msr: buttons: %s\n", names[dev]);
    }
}

} // namespace

// video.c: the input just used. pad = 0: the keyboard or mouse, else a pad of SDL type sdl_type.
extern "C" void msr_input_device(int pad, int sdl_type) {
    static bool named;
    if (!named) {
        named = true;
        G::key_name.store(KeyName); G::pad_name.store(PadName); G::role_name.store(RoleName);
    }
    if (const u32 f = Forced()) { Apply(f); return; }
    if (pad) device_type = static_cast<u32>(sdl_type);
    Apply(pad ? DeviceOf(sdl_type) : G::Keyboard);
}
// video.c: a pad was used last (the mouse pointer hides in play)
extern "C" int msr_input_pad_active(void) {
    const u32 dev = G::device.load(std::memory_order_relaxed);
    return dev == G::Xbox || dev == G::PlayStation || dev == G::Nintendo;
}
// the bindings changed (the settings' Controls): the icons follow
extern "C" void msr_input_bindings_changed(void) {
    const u32 dev = G::device.load();
    if (dev != G::Unknown) { G::shown.store(~0u); Apply(dev); }
}
// compositor.cpp, each frame: whether the pad's buttons (input) and its icons follow the menus' layout
extern "C" void msr_buttons_state(int input, int icons) {
    const bool in = menu_input.exchange(input != 0) != (input != 0), ic = menu_icons.exchange(icons != 0) != (icons != 0);
    if (!in && !ic) return;
    static const bool trace = std::getenv("PWR_MSR_TRACE") != nullptr;
    if (trace) std::fprintf(stderr, "msr: buttons: %s layout, icons %s\n", input ? "menus'" : "play", icons ? "menus'" : "play");
    const u32 dev = G::device.load();
    if (ic && dev != G::Unknown) Apply(dev);
}
// input.c: a pad's bottom face button is 3DS A and its right one 3DS B now (the menus' layout)
extern "C" int msr_menu_buttons(void) { return MenuLayout(PadFamily()) ? 1 : 0; }
// video.c, overlay_host.cpp: the pad buttons that choose and go back (SDL's, by position) in touch mode and the settings
extern "C" int msr_pad_choose(void) { return ChooseButton(PadFamily()); }
extern "C" int msr_pad_back(void) { return BackButton(PadFamily()); }
// overlay_host.cpp: the Controls tab's Nintendo menu layout; the icons follow
extern "C" void msr_set_menu_confirm_right(int on) {
    if (confirm_right.exchange(on != 0) == (on != 0)) return;
    const u32 dev = G::device.load();
    if (dev != G::Unknown) Apply(dev);
}
extern "C" int msr_menu_confirm_right(void) { return confirm_right.load() ? 1 : 0; }

// pica_vk.c: whether a texture is one of the game's icon sheets (1-3), 0 if not; by its size, format and the
// CityHash64 of its bytes (only computed for the sheets' sizes). PWR_MSR_TRACE lists the hashes of textures of a
// sheet's size that are not it (a sheet of another release).
extern "C" int msr_glyph_sheet(const uint8_t* level0, uint32_t w, uint32_t h, int fmt) {
    static const int bits[16] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 0, 0};
    static const bool trace = std::getenv("PWR_MSR_TRACE") != nullptr;
    for (std::size_t i = 0; i < G::Sheets.size(); ++i) {
        const auto& sheet = G::Sheets[i];
        if (w != sheet.width || h != sheet.height || static_cast<u32>(fmt) != sheet.format) continue;
        const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(level0), std::size_t{w} * h * bits[fmt & 15] / 8);
        if (hash == sheet.hash) return static_cast<int>(i) + 1;
        static u64 seen[32]; static int nseen;
        bool dup = false; for (int j = 0; j < nseen; ++j) dup |= seen[j] == hash;
        if (trace && !dup && nseen < 32) {
            seen[nseen++] = hash;
            std::fprintf(stderr, "msr: not icon sheet %zu: %ux%u fmt %d hash %016llx\n", i + 1, w, h, fmt, static_cast<unsigned long long>(hash));
        }
    }
    return 0;
}
// the face cells of an upload of sheet (1-3), RGBA rows bottom first, the game's size or a whole multiple (a pack)
extern "C" void msr_glyph_patch(int sheet, uint8_t* rgba, uint32_t w, uint32_t h) {
    if (sheet < 1 || sheet > static_cast<int>(G::Sheets.size())) return;
    const auto& s = G::Sheets[static_cast<std::size_t>(sheet - 1)];
    const G::Image image{rgba, w, h, 4};
    const bool dialog = sheet == DialogSheet;
    const u32 shown = (dialog ? dialog_shown : G::shown).load(), symbols = (dialog ? dialog_symbols : G::symbols).load();
    const u32 letters = G::letters.load(), shoulders = G::shoulder_letters.load();
    if (!G::Fits(s, image) || (shown == G::Identity && !symbols && !letters && !shoulders)) return;
    const G::Icons icons = G::Read(s, image);
    G::Write(s, image, icons, shown);
    G::WriteSymbols(s, image, icons, symbols, letters);
    G::WriteShoulders(s, image, shoulders);
}
extern "C" unsigned msr_glyph_version(void) { return version.load(std::memory_order_relaxed); }
