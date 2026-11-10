/* Samus Returns' own features on msr-native, as the emulator-based build has them (src/msr): the player's settings,
 * the amiibo extras and cheats. C interface for the runtime; the parts live in runtime/msr/ (C++, sharing src/msr's
 * headers). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Reads the launcher's settings (user/msr/settings.ini, [General]) into the settings overlay (overlay_host.cpp), which
   publishes them: the single-screen presentation's options, the extras, the frame rate. Missing keys keep the main
   build's defaults. Returns 0 when the file was read. */
int msr_settings_load(const char *path);
/* Game frame rate the settings ask for (30, 60, 90, 120 or 144). */
int msr_settings_frame_rate(void);
/* The game's language (a 3DS system language: 1 English, 2 French, 5 Spanish, 3 German, 4 Italian, 8 Dutch) and the
   console region it needs (1 USA, 2 Europe: the game shows German, Italian and Dutch only there) */
int msr_settings_language(void);
int msr_settings_region(void);
int msr_settings_wide_culling(void);          /* the game draws all of a wider view (the menu's "Draw only the 3DS's view" off) */
void msr_state_quick_save(void);             /* state.c: the settings' Quick save and Quick load (main thread) */
void msr_state_quick_load(void);
const char *msr_state_message(void);          /* state.c: "Saved", "Loaded"... once, for the screen */
int msr_fxaa(void);                           /* FXAA on the game's picture (#51) */
int msr_smooth_packs(void);                   /* texture packs' textures filtered smoothly (#51) */
int msr_grade(float *v);                      /* brightness, contrast, saturation into v[0..2] (#25); 0 when all are 1 */
float msr_bloom(void);                        /* the player's Bloom setting, 1 = the game's glow as drawn */
int msr_hdr(float *peak, float *paper);       /* HDR on; its brightness in nits (0 = the display's) */
int  msr_settings_save(void);           /* the overlay's player settings back into the settings file; 0 when written */
const char *msr_settings_file(void);    /* settings.ini */

/* msr-native's exit code asking the launcher to start it again (the overlay's Restart) */
#define MSR_EXIT_RESTART 75

/* ---- game mods (mods.cpp, layered_fs.cpp; the launcher's Mods and Textures tabs) ---- */
/* The user folder (ending in '/'), settings.ini and msr-native's folder; then, before the dump is read, the enabled
   mods and texture packs (the packs only when none were given with --texture-pack) */
void msr_mods_init(const char *user, const char *settings, const char *exe);
void msr_mods_boot(int packs_given);
int  msr_mods_exheader(uint8_t *ex, size_t size);             /* 1: the first mod's exheader.bin is in ex */
int  msr_mods_code(uint8_t **code, uint32_t *size, uint32_t room);   /* 1: changed (code.bin, IPS, BPS), -1: does not fit */
/* The RomFS the game reads: the dump's level-3 image, or one rebuilt with the mods' files (msr_romfs_layered) */
int      msr_romfs_open(const char *dump, uint64_t offset, uint64_t size);
int      msr_romfs_layered(void);
uint64_t msr_romfs_size(void);
size_t   msr_romfs_read(uint64_t offset, size_t length, uint8_t *out);

/* Rewrites the game's instructions for the extras that are on (src/msr/cheats.h Patches: galleries, Fusion Mode, the
   Fusion suit, Hard Mode), after .code is in memory. The recompiled code hands each rewritten word to interp.c. */
void msr_apply_patches(void);
/* Once the native code is loaded: the functions holding the words it does not check (patches added since players
   built theirs) run on the interpreter. */
void msr_patches_module_loaded(void);

/* ---- the single 16:9 screen (compositor.cpp; the emulator-based build's DrawMsrScreens) ---- */
/* One quad over the view, in window (drawable) pixels: a region of the bottom screen (style[3] = mode 0 plain,
   1 luma-keyed, 3 drawn over black) or a solid fill (mode 2). msr.frag reads these as they are. */
typedef struct {
    float dst[4];     /* x, y, w, h */
    float src[4];     /* bottom screen u0, v0, u1, v1 (0..1, screen-oriented) */
    float style[4];   /* opacity, key low (mode 3: gain), key high, mode */
    int feather;      /* soft edge in percent of the quad; bits 8-11 keep the left, top, right, bottom edges hard */
    int circle;       /* circular mask radius in percent (0: none) */
    int source;       /* 1: the beam slots as held (video.c's copy of the bottom screen before R, msr_beams_held) */
    int pad;
    float color[4];   /* fill colour */
} MsrQuad;

typedef struct {
    float top[4];     /* where the top screen goes (x, y, w, h): the view, moved aside in the gallery */
    float panel[4];   /* the bottom screen's panel (menus; touches map into it while msr_panel_visible()) */
    const MsrQuad *quads;   /* to draw after the top screen, in order; valid until the next call */
    int count;
} MsrFrame;

void msr_set_single(int on);       /* the single-screen layout is on: widescreen squeeze, backdrop skip, compositor */
int  msr_single(void);
/* Each presented frame: classifies the bottom screen (320x240 RGBA rows, or NULL), advances the fades from the
   host time (s) and the 3DS buttons (Start opens the pause menu), lays out a ww x wh window. */
void msr_compose(const uint8_t *bottom, double now, unsigned buttons, float ww, float wh, MsrFrame *out);
void msr_single_layout(float ww, float wh, float top[4], float panel[4]);
/* R held (missiles): the HUD's beam slots come from video.c's copy of the bottom screen made before (quads with
   source 1); video.c copies the slots' column each frame this is 0 */
int  msr_beams_held(void);
int  msr_panel_visible(void);
/* pica_vk: the clip-space scale of the top screen's world draws (x down the screen, y across: widescreen and zoom) */
void msr_world_scale(float *sx, float *sy);
int msr_world_in_play(void);   /* the compositor's "in play" (the zoom applies) */
void msr_camera_frame(double now);   /* each VBlank: the game's camera gets the field of view the zoomed view needs (camera_fov.cpp) */
float msr_fov_gain(float px);   /* the renderer: how much wider than the game's that field of view is, for a draw with the camera's projection (x scale px) */
int msr_samus_view(float *h, float *v);   /* Samus on the top screen in the game's own view (5:3, zoom 1): h right, v up, -1..1; 0 if none */
int msr_input_pad_active(void);   /* the last input came from a gamepad (glyphs.cpp) */
void msr_input_device(int pad, int sdl_type);   /* video.c: the input just used (0 = keyboard/mouse), for the button names and icons (glyphs.cpp) */
void msr_input_bindings_changed(void);
/* Unless the HUD shows, an Xbox or PlayStation pad chooses with its bottom face button and goes back with its right one
   (glyphs.cpp; input.c input_pad_bound): msr_buttons_state (compositor.cpp, each frame) says whether its buttons and its
   icons follow that layout, msr_menu_buttons whether a pad's buttons do now, msr_pad_choose the SDL button that
   chooses (touch mode, the settings). */
void msr_buttons_state(int input, int icons);
int msr_menu_buttons(void);
int msr_pad_choose(void);
int msr_pad_back(void);   /* the SDL button that goes back there */
/* The Controls tab's Nintendo menu layout (off by default): no menus' layout for any pad; menus choose with the button
   bound to the game's A and go back with the one bound to its B */
void msr_set_menu_confirm_right(int on);
int msr_menu_confirm_right(void);
int msr_glyph_sheet(const uint8_t *level0, uint32_t w, uint32_t h, int fmt);   /* pica_vk.c: one of the face-button icon sheets (1, 2) or 0 */
void msr_glyph_patch(int sheet, uint8_t *rgba, uint32_t w, uint32_t h);
unsigned msr_glyph_version(void);
/* pica_vk: each draw into the bottom screen's target (1 if it is a full-screen backdrop quad of the title menus);
   returns 1 to leave it out. msr_bottom_frame at each display transfer. */
int  msr_skip_bottom_draw(int quad_backdrop);
void msr_bottom_frame(void);
/* The pad's triggers (input_bridge.cpp): 0 = LT, Morph Ball (taps the minimap); 1 = RT, the next beam (taps its slot);
   2 = the beam before (the mouse wheel up). Only while the HUD shows. input.c then holds the tap msr_injected_touch
   gives (1 while it lasts). */
void msr_trigger(int which);
int  msr_injected_touch(int *x, int *y);
/* Touch navigation (src/msr/touch_nav.cpp): a highlight over the touch screen's buttons, moved with the pad or keys */
int  msr_touch_active(void);
void msr_touch_toggle(void);             /* T (keyboard), L3 */
void msr_touch_l3(void);                 /* the left stick's click: only while a menu shows, or to leave */
void msr_touch_step(int dx, int dy);
void msr_touch_finger(int down);         /* press (hold to drag) and lift */
void msr_touch_close(void);
void msr_touch_frame(float stick_x, float stick_y, int any_held);   /* each frame: stick (-1..1, y up), anything held */
int  msr_input_blocked(void);            /* the game gets no pad input (touch navigation, until its closing button is up) */

/* ---- cheats (cheats.cpp; the emulator-based build's ApplyMsrRefills) ---- */
void msr_cheats_frame(double now);   /* each VBlank: the extras' patches follow the settings, refills, setter calls queued */
struct CPU;
int msr_game_hook(struct CPU *c);

/* ---- texture packs (textures.cpp; Azahar's custom textures, as the emulator-based build loads them) ---- */
int  msr_textures_add_pack(const char *dir);   /* the enabled packs, highest priority first, before the game starts */
void msr_textures_start(void);
int  msr_textures_active(void);
/* pica_vk: new bytes of a texture (level 0, w x h, PICA format; content = its hash of all levels): the pack's hash when
   a pack replaces it (its PNG then loads on a worker), else 0. *pending: not known yet (a worker works out the legacy
   hash); ask again later with the same content. */
uint64_t msr_texture_find(const uint8_t *level0, uint32_t w, uint32_t h, int fmt, uint64_t content, int *pending);
/* pica_vk: what prepares a replacement's upload on the workers (make: pixels -> its own handle, any thread; drop: a
   handle never taken). Not with PWR_TEXPACK_SYNC=1. */
void msr_textures_set_stager(void *(*make)(const uint8_t *rgba, uint32_t w, uint32_t h), void (*drop)(void *staged));
/* The replacement's RGBA rows (in pica_vk's order): 2 with its prepared upload in *staged (the caller's now), 1 with
   the pixels only (the caller uploads them; always when !want_staged), 0 while loading or being prepared, -1 when it
   cannot be read */
int  msr_texture_take(uint64_t key, int want_staged, void **staged, const uint8_t **rgba, uint32_t *w, uint32_t *h);
void msr_texture_done(uint64_t key);          /* uploaded: the pixels may go */
void msr_textures_stats(unsigned *textures, unsigned *loaded, unsigned *failed);    /* dispatch.cpp, at the frame limiter's trap: runs the queued calls; 0 when it was ours */

/* kernel.c: the calling thread below the game's, for work no frame waits for (texture decodes, scans, cache saves) */
void k_background_thread(void);

#ifdef __cplusplus
}
#endif
