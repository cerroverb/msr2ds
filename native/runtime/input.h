/* host input -> hid:USER shared memory (input.c) */
#ifndef PWR_INPUT_H
#define PWR_INPUT_H
#include "kernel.h"
void input_bind(KObject *mem, KObject *pad0);   /* hid:USER GetIPCHandles created these */
void input_key(int sdl_keycode, int down);       /* through the bindings below */
void input_pad_sdl(int sdl_button, int down);   /* a game controller's button (SDL_GameControllerButton), through the bindings */
/* The bindings (the settings overlay's Controls tab): the 3DS buttons in its order, then the circle pad's four directions
   (keys or pad buttons; the left stick and the arrows move it too), a key and a pad button each (-1: none) */
enum { C_A, C_B, C_X, C_Y, C_L, C_R, C_ZL, C_ZR, C_START, C_SELECT, C_UP, C_DOWN, C_LEFT, C_RIGHT,
       C_CUP, C_CDOWN, C_CLEFT, C_CRIGHT, C_COUNT };
const char *input_control_label(int c);         /* "A", ..., "D-pad right" */
const char *input_control_key(int c);           /* its settings.ini name */
int *input_key_binds(void);                     /* C_COUNT SDL keycodes */
int *input_pad_binds(void);                     /* C_COUNT SDL_GameControllerButtons */
void input_default_keys(void);
void input_default_pad(void);
int input_key_control(int sdl_keycode);         /* the control bound to it, or -1 */
int input_pad_control(int sdl_button);
int input_pad_control_now(int sdl_button);      /* the same, in the menus' layout while it applies (msr_menu_buttons) */
int input_pad_bound(int c, int menu);           /* the pad button control c has (menu: in the menus' layout), or -1 */
void input_binding_text(int c, char *buf, size_t n);   /* "Key X, Gamepad B" */
void input_bind_key(int c, int sdl_keycode);
void input_bind_pad(int c, int sdl_button);
/* The triggers as buttons a control can be bound to (#35), pressed past the press point (input_trigger_press, % of their
   travel) and let go under 60% of it; ZL and ZR's by default. Beyond SDL's buttons, so a pad button is never one. */
#define PAD_TRIGGER_LEFT 100
#define PAD_TRIGGER_RIGHT 101
void input_set_trigger_press(int percent);       /* 10..90 */
int input_trigger_press(void);                    /* the press point, % */
int input_trigger_pulled(int right, int value, int *held);   /* a trigger's axis (0..32767) -> pressed or not, with *held its state */
void input_pad_button(int pos_button, int down); /* raw joysticks, positional: 0=south 1=east 2=west 3=north 4=L 5=R 6=select 7=start 8..11=dpad up/down/left/right */
void input_pad_release(void);                   /* the pad went away: its buttons come up */
void input_pad_stick(int x, int y);             /* -32768..32767, y up positive */
void input_pad_rstick(int x, int y);            /* the right stick, the same: the D-pad under the option below */
void input_set_right_stick_dpad(int on);        /* the Aeion abilities (D-pad) on the right stick, the pad's D-pad the circle pad */
int input_right_stick_dpad(void);
void input_touch(int down, int x, int y);       /* bottom-screen pixel coordinates */
void input_mouse(int fire, int aim, float x, float y);   /* the mouse in play: Shoot, and Free Aim toward (x, y) (unit, y up) */
void input_update(void);                        /* once per VBlank */
void input_release_all(void);                 /* settings overlay opened: nothing stays held in the game */
int input_stick_active(void);                   /* circle pad deflected (keys or gamepad): flying, for the auto screen layout */
uint32_t input_buttons(void);                   /* current digital buttons (HID bit layout: bit 2 = Select, bit 3 = Start) */
/* free look (freelook.c): the right stick orbits the chase camera when enabled */
void freelook_enable(int on);
void freelook_stick(int x, int y);              /* -32768..32767, y up positive */
/* draw distance multiplier 1..4 (drawdist.c); applied when the game loads its settings */
void drawdist_set(int k);
#endif
