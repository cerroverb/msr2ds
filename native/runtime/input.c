/* hid:USER input: host keyboard/mouse (window events only, never raw devices) -> the HID shared memory the game reads.
 * Shared memory layout (3dbrew "HID Shared Memory"; inferred, not yet checked against the game's reader):
 *   0x00 pad:   s64 tick of last index reset, s64 previous, u32 latest index, ..., 0x1C u32 current buttons,
 *               0x20 s16 circle x, s16 circle y, 0x28 8 entries {u32 current, pressed, released; s16 cx, cy}
 *   0xA8 touch: s64 tick, s64 previous, u32 latest index, ..., 0xC0 raw {u16 x, u16 y, u32 valid},
 *               0xC8 8 entries {u16 x, u16 y, u32 valid}
 * The host updates once per VBlank and signals the PAD0 event. */
#include "input.h"
#include "msr/msr.h"
#include <SDL.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { B_A = 1 << 0, B_B = 1 << 1, B_SELECT = 1 << 2, B_START = 1 << 3, B_RIGHT = 1 << 4, B_LEFT = 1 << 5, B_UP = 1 << 6, B_DOWN = 1 << 7,
       B_R = 1 << 8, B_L = 1 << 9, B_X = 1 << 10, B_Y = 1 << 11, B_TOUCH = 1 << 20,
       B_CRIGHT = 1u << 28, B_CLEFT = 1u << 29, B_CUP = 1u << 30, B_CDOWN = 1u << 31 };
#define CIRCLE_MAX 0x9C

static pthread_mutex_t I = PTHREAD_MUTEX_INITIALIZER;
static KObject *shmem, *pad_event;
static uint32_t keys, prev_buttons;          /* digital buttons from keys */
static int stick_l, stick_r, stick_u, stick_d;   /* the circle pad from keys: bit 0 its bound keys, bit 1 the arrows */
static int touch_down, touch_x, touch_y;
static uint32_t pad_keys;                    /* digital buttons from a gamepad: what its held buttons pressed (pad_recount) */
static int pad_cl, pad_cr, pad_cu, pad_cd;   /* the circle pad from pad buttons bound to it */
static unsigned char pad_held[SDL_CONTROLLER_BUTTON_MAX];   /* each held pad button's control + 1 as at its press (0: none) */
static unsigned char raw_held[12];           /* each held raw joystick button's control + 1 as at its press */
static int rstick_dpad;                      /* the option: the right stick is the D-pad, the pad's D-pad the circle pad */
static uint32_t rstick_bits;                 /* the D-pad direction the right stick holds */
static int pad_x, pad_y;                     /* gamepad stick, -32768..32767 */
static int mouse_fire, mouse_aim;            /* the mouse in play (video.c): Y, and L with the circle pad toward aim_x/y */
static float aim_x, aim_y;

void input_bind(KObject *mem, KObject *pad0) { pthread_mutex_lock(&I); shmem = mem; pad_event = pad0; pthread_mutex_unlock(&I); }

/* The 3DS buttons the player binds (the settings overlay's Controls tab, as the emulator-based build's rows): a key
 * (SDL keycode) and a gamepad button (SDL_GameControllerButton) each, -1 for none. The keyboard's defaults are a PC
 * layout for playing with the mouse (2026-11-03, the user's: "the jump is space, move is classic WASD ... L R become
 * Q E, TAB is the start menu ... left Shift is LT and the C button or the mouse wheel cycles the weapons"): WASD = the
 * circle pad, Space = B (jump), Q/E = L/R, Left Shift = ZL (Morph Ball), C = ZR (the next beam), Tab = Start,
 * Backspace = Select (the settings); F = A (Aeion, confirm), R = X (melee counter), V = Y (shoot; the mouse too),
 * 1/2/3/4 = D-pad up/left/right/down (the Aeion abilities in the order the game gives them). Arrows are the circle pad
 * too and Enter is A where they are not bound. The pad by position (east = A, south = B, north = X, west = Y) in play,
 * the menus' layout below in menus and dialogs, its triggers ZL and ZR, its left stick the circle pad. The left mouse
 * button on the bottom screen touches; in play the left button elsewhere shoots, the right one aims and the wheel
 * changes beams (video.c). ZL and ZR are the triggers' taps (Morph Ball, next beam: msr_trigger); Select opens the
 * settings (the game takes Select as Start), unless video.c's select_layout says otherwise. The right stick can be
 * the D-pad, the pad's D-pad then the circle pad (rstick_dpad, below). */
static const char *const ctrl_label[C_COUNT] = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Start", "Select (settings)",
                                                 "D-pad up", "D-pad down", "D-pad left", "D-pad right",
                                                 "Circle Pad up", "Circle Pad down", "Circle Pad left", "Circle Pad right" };
static const char *const ctrl_key[C_COUNT] = { "a", "b", "x", "y", "l", "r", "zl", "zr", "start", "select", "up", "down", "left", "right",
                                               "circle_up", "circle_down", "circle_left", "circle_right" };
static const uint32_t ctrl_bit[C_COUNT] = { B_A, B_B, B_X, B_Y, B_L, B_R, 0, 0, B_START, B_SELECT, B_UP, B_DOWN, B_LEFT, B_RIGHT, 0, 0, 0, 0 };
#define KEY_DEFAULTS { SDLK_f, SDLK_SPACE, SDLK_r, SDLK_v, SDLK_q, SDLK_e, SDLK_LSHIFT, SDLK_c, SDLK_TAB, SDLK_BACKSPACE, \
                       SDLK_1, SDLK_4, SDLK_2, SDLK_3, SDLK_w, SDLK_s, SDLK_a, SDLK_d }
#define PAD_DEFAULTS { SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_X, \
                       SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1, -1, \
                       SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_DPAD_UP, \
                       SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, -1, -1, -1, -1 }
static const int key_default[C_COUNT] = KEY_DEFAULTS, pad_default[C_COUNT] = PAD_DEFAULTS;
static int key_bind[C_COUNT] = KEY_DEFAULTS, pad_bind[C_COUNT] = PAD_DEFAULTS;
const char *input_control_label(int c) { return c >= 0 && c < C_COUNT ? ctrl_label[c] : ""; }
const char *input_control_key(int c) { return c >= 0 && c < C_COUNT ? ctrl_key[c] : ""; }
int *input_key_binds(void) { return key_bind; }
int *input_pad_binds(void) { return pad_bind; }
void input_default_keys(void) { memcpy(key_bind, key_default, sizeof key_bind); }
void input_default_pad(void) { memcpy(pad_bind, pad_default, sizeof pad_bind); }
/* the control a key or pad button is bound to, or -1 */
int input_key_control(int sym) { for (int c = 0; c < C_COUNT; c++) if (key_bind[c] == sym) return c; return -1; }
int input_pad_control(int button) { for (int c = 0; c < C_COUNT; c++) if (pad_bind[c] == button) return c; return -1; }
/* The menus' layout (2026-11-03, the user: "I want A to mean select/accept and B to mean back, regardless of controller
   type (so X on PS and O for back). but then in game the controls stay that the bottom button remains jump and the east
   buttons remains the action it was"; then "in game it also needs to change too": the dialogs over play): unless the
   HUD shows, if the pad is not a Nintendo one (msr_menu_buttons, glyphs.cpp; a Nintendo pad's labels already are the
   game's layout), the bottom face button is 3DS A (choose) and the right one 3DS B (back), whatever they do in play;
   the buttons bound to A and B for play do nothing there. A button keeps what it pressed until it comes up
   (pad_held), so a menu closing under it does not make it another button. The Controls tab's Nintendo menu layout
   turns this off for every pad (msr_menu_buttons is then 0: menus take the bindings, as play does). */
int input_pad_bound(int c, int menu) {
    if (c < 0 || c >= C_COUNT) return -1;
    if (menu && (c == C_A || c == C_B)) return c == C_A ? SDL_CONTROLLER_BUTTON_A : SDL_CONTROLLER_BUTTON_B;
    const int b = pad_bind[c];
    return menu && (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_B) ? -1 : b;
}
/* The Aeion abilities on the right stick (2026-11-03, the user: "an option that transfers the d-pad functionality for
   selecting aeion abilities to the right control stick and then the d-pad can move like the left stick"; the settings'
   Controls tab, settings.ini [native_controls] right_stick_dpad): the right stick pushed past half way is the 3DS D-pad
   in the one direction it is pushed furthest (input_pad_rstick), and the pad's D-pad buttons move the circle pad,
   whatever they are bound to. Touch navigation still steps with the D-pad (video.c). */
void input_set_right_stick_dpad(int on) { pthread_mutex_lock(&I); rstick_dpad = on != 0; rstick_bits = 0; pthread_mutex_unlock(&I); }
int input_right_stick_dpad(void) { return rstick_dpad; }
static int dpad_circle(int button) {   /* the circle pad direction a pad's D-pad button is under the option, else -1 */
    if (!rstick_dpad) return -1;
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return C_CUP;     case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return C_CDOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return C_CLEFT; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return C_CRIGHT;
    }
    return -1;
}
void input_pad_rstick(int x, int y) {
    const int ax = abs(x), ay = abs(y), m = ax > ay ? ax : ay;
    pthread_mutex_lock(&I);
    uint32_t bit = 0;
    if (rstick_dpad && m >= (rstick_bits ? 11000 : 16000))   /* past half way; let go under a third */
        bit = ax > ay ? (x > 0 ? B_RIGHT : B_LEFT) : (y > 0 ? B_UP : B_DOWN);
    rstick_bits = bit;
    pthread_mutex_unlock(&I);
}
int input_pad_control_now(int button) {
    const int circle = dpad_circle(button);
    if (circle >= 0) return circle;
    const int menu = msr_menu_buttons();
    if (menu && (button == SDL_CONTROLLER_BUTTON_A || button == SDL_CONTROLLER_BUTTON_B)) return button == SDL_CONTROLLER_BUTTON_A ? C_A : C_B;
    const int c = input_pad_control(button);
    return menu && (c == C_A || c == C_B) ? -1 : c;
}
/* "Key X, Gamepad B" */
static const char *pad_name(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return "A"; case SDL_CONTROLLER_BUTTON_B: return "B";
    case SDL_CONTROLLER_BUTTON_X: return "X"; case SDL_CONTROLLER_BUTTON_Y: return "Y";
    case SDL_CONTROLLER_BUTTON_BACK: return "Back"; case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide";
    case SDL_CONTROLLER_BUTTON_START: return "Start"; case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3"; case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB"; case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad down"; case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad right"; case SDL_CONTROLLER_BUTTON_MISC1: return "Share";
    case SDL_CONTROLLER_BUTTON_PADDLE1: return "Paddle 1"; case SDL_CONTROLLER_BUTTON_PADDLE2: return "Paddle 2";
    case SDL_CONTROLLER_BUTTON_PADDLE3: return "Paddle 3"; case SDL_CONTROLLER_BUTTON_PADDLE4: return "Paddle 4";
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad";
    }
    return "button";
}
void input_binding_text(int c, char *buf, size_t n) {
    if (c < 0 || c >= C_COUNT) { snprintf(buf, n, "Not set"); return; }
    char key[64] = "", pad[48] = "";
    if (key_bind[c] >= 0) { const char *k = SDL_GetKeyName(key_bind[c]); snprintf(key, sizeof key, "Key %s", k && *k ? k : "?"); }
    static const char *const dir[4] = { "up", "down", "left", "right" };
    const int own = pad_bind[c] >= 0 && dpad_circle(pad_bind[c]) < 0;   /* a pad button that is still its own */
    if (rstick_dpad && c >= C_UP && c <= C_RIGHT)   /* the option: the right stick (and a button bound here that is not the D-pad) */
        snprintf(pad, sizeof pad, "Gamepad %s%sright stick %s", own ? pad_name(pad_bind[c]) : "", own ? " / " : "", dir[c - C_UP]);
    else if (rstick_dpad && c >= C_CUP)   /* the option: the D-pad (and the left stick, as always) */
        snprintf(pad, sizeof pad, "Gamepad %s%sD-pad %s", own ? pad_name(pad_bind[c]) : "", own ? " / " : "", dir[c - C_CUP]);
    else if (pad_bind[c] >= 0) snprintf(pad, sizeof pad, "Gamepad %s", pad_name(pad_bind[c]));
    else if (c == C_ZL || c == C_ZR) snprintf(pad, sizeof pad, "Gamepad %s trigger", c == C_ZL ? "left" : "right");
    snprintf(buf, n, "%s%s%s", key, key[0] && pad[0] ? ", " : "", pad[0] || key[0] ? pad : "Not set");
}
/* a key or pad button for control c: taken from any other control that had it */
void input_bind_key(int c, int sym) {
    if (c < 0 || c >= C_COUNT) return;
    for (int o = 0; o < C_COUNT; o++) if (key_bind[o] == sym) key_bind[o] = -1;
    key_bind[c] = sym; input_release_all();
}
void input_bind_pad(int c, int button) {
    if (c < 0 || c >= C_COUNT) return;
    for (int o = 0; o < C_COUNT; o++) if (pad_bind[o] == button) pad_bind[o] = -1;
    pad_bind[c] = button; input_release_all();
}
static void set_bit(int *v, int bit, int down) { *v = down ? *v | bit : *v & ~bit; }
void input_key(int sym, int down) {
    int c = input_key_control(sym), bit = 1;
    if (c < 0) {   /* not bound: the arrows move the circle pad, Enter is A (menus) */
        bit = 2;
        switch (sym) {
        case SDLK_LEFT: c = C_CLEFT; break;  case SDLK_RIGHT: c = C_CRIGHT; break;
        case SDLK_UP: c = C_CUP; break;      case SDLK_DOWN: c = C_CDOWN; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: c = C_A; break;
        default: return;
        }
    }
    pthread_mutex_lock(&I);
    switch (c) {
    case C_CLEFT: set_bit(&stick_l, bit, down); break;  case C_CRIGHT: set_bit(&stick_r, bit, down); break;
    case C_CUP: set_bit(&stick_u, bit, down); break;    case C_CDOWN: set_bit(&stick_d, bit, down); break;
    default: if (ctrl_bit[c]) keys = down ? keys | ctrl_bit[c] : keys & ~ctrl_bit[c]; break;
    }
    pthread_mutex_unlock(&I);
}
/* under I: the pad's buttons and its circle pad from what each held button pressed */
static void pad_recount(void) {
    pad_keys = 0; pad_cu = pad_cd = pad_cl = pad_cr = 0;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX + 12; b++) {
        const int c = (b < SDL_CONTROLLER_BUTTON_MAX ? pad_held[b] : raw_held[b - SDL_CONTROLLER_BUTTON_MAX]) - 1;
        if (c == C_CUP) pad_cu = 1; else if (c == C_CDOWN) pad_cd = 1; else if (c == C_CLEFT) pad_cl = 1; else if (c == C_CRIGHT) pad_cr = 1;
        else if (c >= 0) pad_keys |= ctrl_bit[c];
    }
}
/* a gamepad button through the bindings (controllers with an SDL mapping), the menus' layout in menus */
void input_pad_sdl(int button, int down) {
    if (button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX) return;
    const int c = down ? input_pad_control_now(button) : -1;
    pthread_mutex_lock(&I);
    pad_held[button] = (unsigned char)(c + 1);
    pad_recount();
    pthread_mutex_unlock(&I);
}
/* raw joysticks (no SDL mapping, video.c): positional layout, south = 3DS B, east = A, west = Y, north = X; in menus
   south = A and east = B, and under the right stick option the hat moves the circle pad, as the mapped pads */
void input_pad_button(int b, int down) {
    static const int map[12] = { C_B, C_A, C_Y, C_X, C_L, C_R, C_SELECT, C_START, C_UP, C_DOWN, C_LEFT, C_RIGHT };
    if (b < 0 || b >= 12) return;
    int c = map[b];
    if (down && b < 2 && msr_menu_buttons()) c = b == 0 ? C_A : C_B;
    if (down && b >= 8 && rstick_dpad) c = C_CUP + (b - 8);
    pthread_mutex_lock(&I);
    raw_held[b] = down ? (unsigned char)(c + 1) : 0;
    pad_recount();
    pthread_mutex_unlock(&I);
}
/* the pad in use went away: none of its buttons stay held */
void input_pad_release(void) {
    pthread_mutex_lock(&I); memset(pad_held, 0, sizeof pad_held); memset(raw_held, 0, sizeof raw_held); pad_recount(); pthread_mutex_unlock(&I);
}

void input_pad_stick(int x, int y) { pthread_mutex_lock(&I); pad_x = x; pad_y = y; pthread_mutex_unlock(&I); }

/* the mouse in play (video.c): fire = the left button (3DS Y, Shoot); aim = the right one, Free Aim (L held) with the
   circle pad all the way toward (x, y), a unit vector, y up */
void input_mouse(int fire, int aim, float x, float y) {
    pthread_mutex_lock(&I); mouse_fire = fire; mouse_aim = aim; aim_x = x; aim_y = y; pthread_mutex_unlock(&I);
}
void input_touch(int down, int x, int y) {
    static int tr = -1; if (tr < 0) tr = getenv("PWR_INPUT_TRACE") != NULL;
    if (tr) fprintf(stderr, "input: touch %s %d,%d\n", down ? "down" : "up", x, y);
    pthread_mutex_lock(&I);
    touch_down = down && x >= 0 && x < 320 && y >= 0 && y < 240;
    if (touch_down) { touch_x = x; touch_y = y; }
    pthread_mutex_unlock(&I);
}

void input_release_all(void) {
    pthread_mutex_lock(&I); keys = pad_keys = 0; stick_l = stick_r = stick_u = stick_d = 0; pad_cl = pad_cr = pad_cu = pad_cd = 0;
    memset(pad_held, 0, sizeof pad_held); memset(raw_held, 0, sizeof raw_held); rstick_bits = 0;
    pad_x = pad_y = 0; touch_down = 0;
    mouse_fire = mouse_aim = 0; pthread_mutex_unlock(&I);
}
int input_stick_active(void) {
    pthread_mutex_lock(&I); int a = stick_l || stick_r || stick_u || stick_d || pad_cl || pad_cr || pad_cu || pad_cd ||
                                    abs(pad_x) > 6000 || abs(pad_y) > 6000; pthread_mutex_unlock(&I);
    return a;
}
static uint32_t mouse_buttons(void) { return (mouse_fire ? B_Y : 0) | (mouse_aim ? B_L : 0); }
uint32_t input_buttons(void) { pthread_mutex_lock(&I); uint32_t b = keys | pad_keys | rstick_bits | mouse_buttons(); pthread_mutex_unlock(&I); return b; }

void input_update(void) {
    pthread_mutex_lock(&I);
    uint32_t base = shmem ? shmem->addr : 0;
    if (base < 0x1000) { pthread_mutex_unlock(&I); return; }   /* not mapped by the game yet */
    int cx = ((stick_r || pad_cr) - (stick_l || pad_cl)) * CIRCLE_MAX, cy = ((stick_u || pad_cu) - (stick_d || pad_cd)) * CIRCLE_MAX;
    if (!cx && !cy && (abs(pad_x) > 6000 || abs(pad_y) > 6000)) { cx = pad_x * CIRCLE_MAX / 32768; cy = pad_y * CIRCLE_MAX / 32768; }
    if (mouse_aim) { cx = (int)lrintf(aim_x * CIRCLE_MAX); cy = (int)lrintf(aim_y * CIRCLE_MAX); }
    uint32_t btn = keys | pad_keys | rstick_bits | mouse_buttons();
    if (msr_input_blocked()) { btn = 0; cx = cy = 0; }   /* Samus Returns' touch navigation has the pad (msr/compositor.cpp) */
    if (cx > 0x28) btn |= B_CRIGHT; if (cx < -0x28) btn |= B_CLEFT;
    if (cy > 0x28) btn |= B_CUP;    if (cy < -0x28) btn |= B_CDOWN;
    int td = touch_down, tx = touch_x, ty = touch_y;
    if (msr_injected_touch(&tx, &ty)) td = 1;   /* Samus Returns: a beam or Morph Ball tap (msr/compositor.cpp) */
    else { tx = touch_x; ty = touch_y; }
    if (td) btn |= B_TOUCH;
    uint64_t now = k_ticks();
    /* pad ring */
    uint32_t idx = (rd32(base + 0x10) + 1) & 7;
    if (idx == 0) { uint64_t t = rd32(base) | (uint64_t)rd32(base + 4) << 32; wr32(base + 8, (uint32_t)t); wr32(base + 12, (uint32_t)(t >> 32)); wr32(base, (uint32_t)now); wr32(base + 4, (uint32_t)(now >> 32)); }
    uint32_t e = base + 0x28 + idx * 0x10;
    wr32(e, btn); wr32(e + 4, btn & ~prev_buttons); wr32(e + 8, ~btn & prev_buttons);
    wr16(e + 12, (uint32_t)cx); wr16(e + 14, (uint32_t)cy);
    wr32(base + 0x1C, btn); wr16(base + 0x20, (uint32_t)cx); wr16(base + 0x22, (uint32_t)cy);
    wr32(base + 0x10, idx);
    prev_buttons = btn;
    /* touch ring */
    uint32_t tb = base + 0xA8, ti = (rd32(tb + 0x10) + 1) & 7;
    if (ti == 0) { uint64_t t = rd32(tb) | (uint64_t)rd32(tb + 4) << 32; wr32(tb + 8, (uint32_t)t); wr32(tb + 12, (uint32_t)(t >> 32)); wr32(tb, (uint32_t)now); wr32(tb + 4, (uint32_t)(now >> 32)); }
    uint32_t te = tb + 0x20 + ti * 8;
    wr16(te, td ? (uint32_t)tx : 0); wr16(te + 2, td ? (uint32_t)ty : 0); wr32(te + 4, (uint32_t)td);
    wr16(tb + 0x18, td ? (uint32_t)tx : 0); wr16(tb + 0x1A, td ? (uint32_t)ty : 0); wr32(tb + 0x1C, (uint32_t)td);
    wr32(tb + 0x10, ti);
    KObject *ev = pad_event;
    pthread_mutex_unlock(&I);
    if (ev) k_signal(ev);
}
