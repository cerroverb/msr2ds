/* msr-native: Metroid: Samus Returns without an emulator, in the style of Woohoo! Flight School.
 * The game's ARM code runs from the module the launcher builds from the player's own dump (tools/msr/recomp,
 * msr_aot.dll / libmsr_aot.so); the 3DS kernel, services, GPU and DSP are this runtime (from Woohoo! Flight School).
 *
 * Started on its own (no options, or --launcher) it is the launcher (msr/launcher.cpp): the settings menu with the
 * game file, the native code's build and Play, which starts this program again with --play and the options below.
 * --direct (msr-native-direct.exe's): Play without the launcher, which opens only when the game cannot start yet.
 *
 * usage: msr-native [--play] [--dump <dump.3ds|.cci|.cxi>] [--module <library>] [options]
 *   --dump        the decrypted dump (also $MSR_DUMP, or the first line of user/msr/game-path.txt)
 *   --module      the recompiled code (default user/msr/recomp/msr_aot.dll, libmsr_aot.so on Linux)
 *   --input=none  never open real input devices (headless runs)    --mute  no audio output
 *   --scale=N     initial window scale (default 2); display options as in video.c (--layout=..., --filter=...)
 *   --fps=N       30, 60, 90, 120 or 144 frames per second (default: the settings' frame rate)
 *   --preload-shaders=off   do not build the shaders of earlier sessions at start-up (pwr_shaders.bin)
 *   --settings    the player's settings (default user/msr/settings.ini, or ../msr/settings.ini from user/native)
 *   --texture-pack <folder>   a texture pack to use (repeatable, highest priority first; the launcher passes the enabled ones)
 * Save data goes to $PWR_SAVE_DIR (default user/native-save). PWR_TRACE=1 logs kernel calls and IPC.
 * The guest main thread runs on its own host thread; this thread owns the window and drives VBlank at 60 Hz. */
#include "gpu.h"
#include "runtime.h"
#include "dump.h"
#include "input.h"
#include "msr/msr.h"
#include <SDL_filesystem.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#else
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include "android.h"
#include <SDL_hints.h>
#include <SDL_main.h>   /* main is SDL_main there, which SDL's Java side runs (android_main below) */
#endif

int g_input_none, g_mute;
extern const char *g_romfs_path;
extern uint64_t g_romfs_offset, g_romfs_size;

/* $PWR_SETTINGS (the profile's settings.json, which the F1 overlay writes) as starting options; the command line, which
   comes after, wins. Flat JSON: "key": value pairs; the display options plus scale and mute. */
static void settings_file(int *scale) {
    const char *path = getenv("PWR_SETTINGS"); FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) return;
    char buf[8192]; size_t len = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[len] = 0;
    for (char *c = buf; *c; ) {
        char *k = strchr(c, '"'); if (!k) break; char *ke = strchr(k + 1, '"'); if (!ke) break;
        char *v = strchr(ke, ':'); if (!v) break; v++;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
        char *ve = v; if (*v == '"') { v++; ve = strchr(v, '"'); if (!ve) break; } else while (*ve && !strchr(",}\r\n", *ve)) ve++;
        char name[32], val[64];
        snprintf(name, sizeof name, "%.*s", (int)(ke - k - 1), k + 1); snprintf(val, sizeof val, "%.*s", (int)(ve - v), v);
        if (!strcmp(name, "scale")) { if (atoi(val) > 0) *scale = atoi(val); }
        else if (!strcmp(name, "mute")) { if (!strcmp(val, "true")) g_mute = 1; }
        else video_option(name, val);   /* unknown keys (args, ...) are ignored */
        c = *ve == '"' ? ve + 1 : ve;
    }
}

/* ---- the dump: NCSD/NCCH, ExeFS .code (BLZ), ExHeader, RomFS ---- */
#define MEDIA 0x200u
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/* Nintendo's backwards LZ77 (the ExeFS .code compression), in place in a buffer of the decompressed size */
static int blz_decompress(uint8_t *buf, uint32_t packed, uint32_t *out_size) {
    if (packed < 8) return -1;
    uint32_t end_info = le32(buf + packed - 8), extra = le32(buf + packed - 4);
    uint32_t header = end_info >> 24, compressed = end_info & 0xFFFFFF;
    uint32_t total = packed + extra;
    int64_t rd = (int64_t)packed - header, wr = total, stop = (int64_t)packed - compressed;
    while (rd > stop) {
        uint8_t flags = buf[--rd];
        for (int i = 0; i < 8 && rd > stop; i++, flags <<= 1) {
            if (flags & 0x80) {
                rd -= 2; uint32_t pair = buf[rd] | (uint32_t)buf[rd + 1] << 8;
                uint32_t len = ((pair >> 12) & 0xF) + 3, dist = (pair & 0xFFF) + 2;
                for (uint32_t k = 0; k < len; k++) { wr--; buf[wr] = buf[wr + dist + 1]; }
            } else { wr--; rd--; buf[wr] = buf[rd]; }
        }
    }
    *out_size = total;
    return 0;
}

/* the game file, a plain or a compressed one (dump.c) */
static int read_at(DumpFile *f, uint64_t off, void *dst, size_t n) { return dump_read(f, off, dst, n); }

/* the code as the dump has it and as the game mods left it, when they changed it (rt_code_changed) */
static uint8_t *g_code_orig, *g_code_mod; static uint32_t g_code_orig_size, g_code_mod_size, g_code_base;

/* loads .code at its text address and the main thread's priority; finds the RomFS */
uint32_t g_text_lo, g_text_hi;   /* the game's code (save states leave it out: state.c) */
uint64_t g_code_hash;            /* the program as loaded, with the game mods' code: a save state must come from the same */
static int load_dump(const char *path) {
    DumpFile *f = dump_open(path);   /* kept as g_dump: fs.c reads the RomFS through it */
    if (!f) { fprintf(stderr, "native: cannot open %s\n", path); return -1; }
    uint8_t h[0x200]; uint64_t ncch = 0;
    if (read_at(f, 0, h, sizeof h)) goto bad;
    if (!memcmp(h + 0x100, "NCSD", 4)) { ncch = (uint64_t)le32(h + 0x120) * MEDIA; if (read_at(f, ncch, h, sizeof h)) goto bad; }
    if (memcmp(h + 0x100, "NCCH", 4)) { fprintf(stderr, "native: %s is not a 3DS game dump\n", path); dump_close(f); return -1; }
    if (!(h[0x188 + 7] & 4)) { fprintf(stderr, "native: the dump is encrypted; decrypt it first\n"); dump_close(f); return -1; }
    if (le64(h + 0x118) != 0x00040000001BB200ull) fprintf(stderr, "native: warning: program %016llx is not Samus Returns (USA)\n", (unsigned long long)le64(h + 0x118));
    uint8_t ex[0x800];
    if (read_at(f, ncch + 0x200, ex, sizeof ex)) goto bad;
    msr_mods_exheader(ex, sizeof ex);   /* a game mod's exheader.bin (segment sizes, services) */
    g_main_prio = ex[0x20F];
    uint32_t text = le32(ex + 0x10), exefs = le32(h + 0x1A0) * MEDIA;
    /* the program's memory map for svcQueryMemory: text (RX), rodata (R), data + bss (RW), the main thread's stack */
    k_add_region(text, le32(ex + 0x14) * 0x1000u, 5, 4);
    k_add_region(le32(ex + 0x20), le32(ex + 0x24) * 0x1000u, 1, 4);
    k_add_region(le32(ex + 0x30), le32(ex + 0x34) * 0x1000u + ((le32(ex + 0x3C) + 0xfff) & ~0xfffu), 3, 5);
    k_add_region(0x10000000u - le32(ex + 0x1C), le32(ex + 0x1C), 3, 5);
    uint8_t eh[0x200];
    if (read_at(f, ncch + exefs, eh, sizeof eh)) goto bad;
    for (int i = 0; i < 10; i++) {
        if (memcmp(eh + i * 16, ".code\0\0\0", 8)) continue;
        uint32_t off = le32(eh + i * 16 + 8), size = le32(eh + i * 16 + 12);
        uint8_t *buf = malloc(0x2000000);
        if (!buf || size > 0x2000000 || read_at(f, ncch + exefs + 0x200 + off, buf, size)) { free(buf); goto bad; }
        uint32_t n = size;
        if ((ex[0xD] & 1) && blz_decompress(buf, size, &n)) { free(buf); goto bad; }
        {   /* the game mods' code.bin or patches (BPS, IPS), over the code and its .bss pages; the words they change run on
               the interpreter (dispatch.cpp rt_code_changed, once the module is loaded) */
            uint32_t room = n + ((le32(ex + 0x3C) + 0xfff) & ~0xfffu), modded = n;
            if (room > 0x2000000) room = 0x2000000;
            uint8_t *orig = malloc(n);
            if (orig) memcpy(orig, buf, n);
            int r = msr_mods_code(&buf, &modded, room);
            if (r < 0) { free(orig); free(buf); dump_close(f); fprintf(stderr, "native: a game mod's code patch does not fit this game\n"); return -1; }
            if (r > 0 && orig) { g_code_orig = orig; g_code_orig_size = n; g_code_mod = malloc(modded); if (g_code_mod) memcpy(g_code_mod, buf, modded); g_code_mod_size = modded; g_code_base = text; }
            else free(orig);
            n = modded;
        }
        rt_mem_commit(text, n);
        memcpy(g_mem + text, buf, n);
        g_text_lo = text; g_text_hi = text + le32(ex + 0x14) * 0x1000u;
        g_code_hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < n; i++) g_code_hash = (g_code_hash ^ buf[i]) * 1099511628211ull;
        free(buf);
        fprintf(stderr, "native: .code %u bytes at %08x, main thread priority %d\n", n, text, g_main_prio);
        goto code_done;
    }
    fprintf(stderr, "native: no .code in the ExeFS\n"); dump_close(f); return -1;
code_done:;
    /* RomFS: IVFC header, master hash, then level 3 (the file system) at the next level-3 block boundary */
    uint64_t romfs = ncch + (uint64_t)le32(h + 0x1B0) * MEDIA;
    uint8_t iv[0x60];
    if (read_at(f, romfs, iv, sizeof iv) || memcmp(iv, "IVFC", 4)) { fprintf(stderr, "native: no RomFS in the dump\n"); dump_close(f); return -1; }
    uint32_t master = le32(iv + 0x08), block = 1u << le32(iv + 0x4C);
    g_romfs_offset = romfs + ((0x60u + master + block - 1) & ~(uint64_t)(block - 1));
    g_romfs_size = le64(iv + 0x44);
    g_romfs_path = path;
    fprintf(stderr, "native: RomFS level 3 at %llx, %llu bytes\n", (unsigned long long)g_romfs_offset, (unsigned long long)g_romfs_size);
    if (msr_romfs_open(path, g_romfs_offset, g_romfs_size)) { dump_close(f); return -1; }   /* with the game mods' files (fs.c reads it) */
    g_dump = f;
    return 0;
bad:
    fprintf(stderr, "native: %s is damaged or truncated\n", path); dump_close(f); return -1;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* 60, 90, 120 or 144 FPS, as the emulator-based build does it (src/msr/frame_rate.h): the main loop's 33 ms limit (cmp r0,
   #33 and rsb r6, r0, #33) becomes FrameRate::LimitMs (16, 11, 8 or 6 ms), so the VBlank wait at that rate sets the pace (main()
   runs VBlank at the rate above 60), and the interface parts that step a fixed 1/30 s per frame take the measured frame
   time. Everything else in the game already advances by measured time, and this runtime's ticks are the host's clock, so
   the emulator's faster CPU clock has no counterpart here. The recompiled code notices the rewritten words and runs
   them through interp.c. Returns the rate in effect. */
static int set_frame_rate(int fps) {
    static int applied;   /* 1 once the words were checked and changed: then they may be changed again, or put back */
    uint32_t limit = (uint32_t)((1000 + (fps > 30 ? fps : 60) - 1) / (fps > 30 ? fps : 60) - 1);
    const struct { uint32_t addr, original, replacement; } patches[] = {
        { 0x156484, 0xE3500021, 0xE3500000 | limit },   /* cmp r0, #33 -> #limit */
        { 0x15648C, 0xE2606021, 0xE2606000 | limit },   /* rsb r6, r0, #33 -> #limit */
        { 0x49DFF8, 0xED9F8AA6, 0xEEB08A40 },           /* the HUD: vldr s16, =1/30 -> vmov.f32 s16, s0 */
        { 0x4E3BA4, 0xED9F8AD5, 0xE1A00000 },           /* an interface class: vldr s16, =1/30 -> mov r0, r0 */
        { 0x10C6B4, 0x1D940AD0, 0xE1A00000 },           /* the interface's fixed-step mode: vldrne s0, [r4, #0x340] -> nop */
    };
    if (fps <= 30 && !applied) return 30;
    if (!applied) {
        for (size_t i = 0; i < sizeof patches / sizeof *patches; i++)
            if (rd32(patches[i].addr) != patches[i].original) {
                fprintf(stderr, "native: the frame-rate patch does not fit this program (%08x): staying at 30 FPS\n", patches[i].addr);
                return 30;
            }
        applied = 1;
    }
    for (size_t i = 0; i < sizeof patches / sizeof *patches; i++) wr32(patches[i].addr, fps <= 30 ? patches[i].original : patches[i].replacement);
    if (fps <= 30) { fprintf(stderr, "native: 30 FPS\n"); return 30; }
    fprintf(stderr, "native: %d FPS (frame limit %u ms)\n", fps, limit);
    return fps;
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

/* The display's refresh, when the frame rate is that refresh (144 FPS on a 143.86 Hz monitor): VBlank then follows the
   monitor's own vblanks, 2 ms ahead of each, instead of the host clock's exact 144 Hz. With the latter the two drifted
   apart by a frame every few seconds, which a mailbox swapchain shows as a frame dropped or repeated (a hitch every ~7 s).
   Windows (DWM) gives the refresh period and the time of the last vblank. PWR_NO_DISPLAY_SYNC=1: the host clock. */
#ifdef _WIN32
#include <dwmapi.h>
static int display_timing(double *period, double *vblank) {
    DWM_TIMING_INFO ti; memset(&ti, 0, sizeof ti); ti.cbSize = sizeof ti;
    if (DwmGetCompositionTimingInfo(NULL, &ti) != S_OK || !ti.qpcRefreshPeriod || !ti.qpcVBlank) return 0;
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    double now = now_s();
    *period = (double)ti.qpcRefreshPeriod / (double)f.QuadPart;
    *vblank = now - (double)(c.QuadPart - (LONGLONG)ti.qpcVBlank) / (double)f.QuadPart;
    return *period > 1.0 / 400 && *period < 1.0 / 20;
}
#else
static int display_timing(double *period, double *vblank) { (void)period; (void)vblank; return 0; }
#endif
/* the next VBlank tick after one at `next`: the frame period on the host clock, or the display's next vblank less 2 ms */
static double next_tick(double next, int fps) {
    double step = fps > 60 ? 1.0 / fps : 1.0 / 60.0;
    static int off = -1; if (off < 0) off = getenv("PWR_NO_DISPLAY_SYNC") != NULL;
    static double period, vblank, checked; static int ok, logged;
    double t = now_s();
    if (!off && t - checked > 0.5) { checked = t; ok = display_timing(&period, &vblank); }
    if (off || !ok || fabs(period - step) > step * 0.015) return next + step;
    if (!logged) { logged = 1; fprintf(stderr, "native: VBlank follows the display (%.3f Hz)\n", 1.0 / period); }
    const double lead = 0.002, want = next + step + lead;   /* the display vblank nearest to one frame on, less the lead */
    double k = floor((want - vblank) / period + 0.5);
    double tick = vblank + k * period - lead;
    if (tick < next + step * 0.5) tick += period;   /* never two ticks within half a frame */
    return tick;
}


/* the settings overlay: the frame rate while playing, Restart (MSR_EXIT_RESTART: the launcher starts it again) and Quit */
static volatile int g_fps = 60, g_exit = -1;
static int fps_choice(int fps) { return fps >= 144 ? 144 : fps >= 120 ? 120 : fps >= 90 ? 90 : fps >= 60 ? 60 : 30; }
void main_set_frame_rate(int fps) {
    fps = fps_choice(fps);
    g_fps = set_frame_rate(fps);
    video_set_fps(g_fps);
}
void main_exit(int code) { g_exit = code; }

/* the folder holding a file ("a/b/c.ini" -> "a/b/"), "" for none */
static void folder_of(const char *path, char *out, size_t n) {
    const char *slash = strrchr(path, '/'), *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    snprintf(out, n, "%.*s", slash ? (int)(slash - path + 1) : 0, path);
}

/* The log (stderr, a file) is buffered and written every 2 s with the stats: unbuffered, each line cost milliseconds of
   writes (about 19 ms for the 2 s stats) on the thread that runs VBlank, a 26 ms frame and a catch-up frame every 2 s
   at 144 FPS (2026-11-02). It still reaches the file on exit, abort() and a crash. */
static void log_flush_on_abort(int sig) { (void)sig; fflush(stderr); }
#ifdef _WIN32
static LONG WINAPI log_flush_on_crash(EXCEPTION_POINTERS *e) { (void)e; fflush(stderr); return EXCEPTION_CONTINUE_SEARCH; }
#endif
static void log_buffered(void) {
    if (getenv("PWR_LOG_UNBUFFERED")) return;   /* every line at once, as before (debugging a hang without a stats line) */
    setvbuf(stderr, NULL, _IOFBF, 1 << 16);
    signal(SIGABRT, log_flush_on_abort);
#ifdef _WIN32
    SetUnhandledExceptionFilter(log_flush_on_crash);
#endif
}

int msr_launcher_main(void);
int msr_launcher_direct(void);

#ifdef __ANDROID__
/* The Android app runs one of the two in each process (android.c): the launcher (no arguments), whose Play starts the
   app again with the game's arguments, or the game, after which the app comes back as the launcher (Quit, the game's
   end, or a failure to start), or as the game again (Restart). Closed by the player (the window), it stays closed. */
static int g_android_argc; static const char *const *g_android_argv;
static void android_after_game(int relaunch_game, int relaunch_launcher) {
    if (relaunch_game) msr_android_relaunch(g_android_argc - 1, g_android_argv + 1);
    if (relaunch_launcher) msr_android_relaunch(0, NULL);
}
static int msr_main(int argc, char **argv);
int main(int argc, char **argv) {   /* SDL_main */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");   /* Back is the menu's (input.c), not the end of the app */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (chdir(msr_android_root())) return 1;   /* the paths are user/..., as in the PC's folder */
    if (argc > 1) {
        mkdir("user", 0777);
        if (freopen("user/msr-native.log", "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);   /* the launcher's is its own */
    }
    g_android_argc = argc; g_android_argv = (const char *const *)argv;
    const int r = msr_main(argc, argv);
    if (argc > 1) android_after_game(0, 1);   /* it could not start (the log says why) */
    return r;
}
#undef main
#define main msr_main
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    {   /* a window program: from a console without a redirection, the log goes to that console */
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        if (argc > 1 && (h == NULL || h == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) freopen("CONOUT$", "w", stderr);
    }
#endif
    log_buffered();
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--launcher"))) return msr_launcher_main();
    if (argc == 2 && !strcmp(argv[1], "--direct")) return msr_launcher_direct();
    int scale = 2, fps = 0, volume = 100, packs_given = 0;
    const char *dump = getenv("MSR_DUMP"), *module = NULL, *settings = NULL;
    static char game_path[4096], module_path[4096], save_path[4096];
    settings_file(&scale);
#ifdef _WIN32
    timeBeginPeriod(1);   /* sleeps (the 60 Hz loop, the DSP clock's ~5 ms ticks) otherwise round up to the 15.6 ms tick */
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) continue;   /* the launcher's: the game itself */
        else if (!strcmp(argv[i], "--input=none")) g_input_none = 1;
        else if (!strcmp(argv[i], "--mute")) g_mute = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else if (!strcmp(argv[i], "--module") && i + 1 < argc) module = argv[++i];
        else if (!strcmp(argv[i], "--settings") && i + 1 < argc) settings = argv[++i];
        else if (!strcmp(argv[i], "--texture-pack") && i + 1 < argc) { msr_textures_add_pack(argv[++i]); packs_given = 1; }
        else if (!strncmp(argv[i], "--volume=", 9)) { volume = atoi(argv[i] + 9); volume = volume < 0 ? 0 : volume > 100 ? 100 : volume; }
        else if (!strncmp(argv[i], "--scale=", 8)) scale = atoi(argv[i] + 8) > 0 ? atoi(argv[i] + 8) : 2;
        else if (!strncmp(argv[i], "--fps=", 6)) fps = atoi(argv[i] + 6);
        else if (!strcmp(argv[i], "--preload-shaders=off")) { extern void pica_vk_preload(int on); pica_vk_preload(0); }   /* the launcher's "Preload shaders" */
        else if (!strncmp(argv[i], "--", 2) && strchr(argv[i], '=')) {   /* display options: --name=value -> video_option */
            char name[32]; const char *eq = strchr(argv[i], '='); size_t n = (size_t)(eq - argv[i] - 2);
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, argv[i] + 2, n); name[n] = 0; for (char *c = name; *c; c++) if (*c == '-') *c = '_';
            if (video_option(name, eq + 1)) { fprintf(stderr, "native: unknown option or value %s\n", argv[i]); return 2; }
        }
        else { fprintf(stderr, "native: unknown option %s\n", argv[i]); return 2; }
    }
    if (!dump) {   /* the launcher's choice */
        FILE *g = fopen("user/msr/game-path.txt", "rb");
        if (g) { size_t n = fread(game_path, 1, sizeof game_path - 1, g); fclose(g); game_path[n] = 0;
                 game_path[strcspn(game_path, "\r\n")] = 0; if (game_path[0]) dump = game_path; }
    }
    if (!dump) { fprintf(stderr, "native: no dump (--dump <path>)\n"); return 2; }
    if (!module) {
#ifdef _WIN32
        snprintf(module_path, sizeof module_path, "user/msr/recomp/msr_aot.dll");
#elif defined(__ANDROID__)
        snprintf(module_path, sizeof module_path, "user/msr/recomp-android/libmsr_aot.so");   /* built on a PC */
#elif defined(__APPLE__)
        snprintf(module_path, sizeof module_path, "user/msr/recomp/libmsr_aot.dylib");
#else
        snprintf(module_path, sizeof module_path, "user/msr/recomp/libmsr_aot.so");
#endif
        module = module_path;
    }
    if (!getenv("PWR_SAVE_DIR")) {
        snprintf(save_path, sizeof save_path, "PWR_SAVE_DIR=%s", "user/native-save");
        putenv(save_path);
    }
    if (!settings) settings = exists("user/msr/settings.ini") ? "user/msr/settings.ini" : "../msr/settings.ini";
    msr_settings_load(settings);
    {   /* the game mods and texture packs, found as the launcher finds them: <user>/mods, <user>/msr/settings.ini */
        char msr_dir[4096], user_dir[4096];
        folder_of(settings, msr_dir, sizeof msr_dir);
        snprintf(user_dir, sizeof user_dir, "%s..", msr_dir[0] ? msr_dir : "./");
        char *base = SDL_GetBasePath();
        msr_mods_init(user_dir, settings, base ? base : "");
        if (base) SDL_free(base);
        msr_mods_boot(packs_given);
    }
    {   /* code the module lacks goes beside the settings, where the launcher's next build of the native code reads it */
        static char entries[4096]; const char *slash = strrchr(settings, '/'), *bslash = strrchr(settings, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
        snprintf(entries, sizeof entries, "%.*srecomp-jit-entries-core-native.txt", slash ? (int)(slash - settings + 1) : 0, settings);
        rt_set_entries_file(entries);
    }
    if (!fps) fps = msr_settings_frame_rate();
    fps = fps_choice(fps);
    if (rt_mem_init()) { fprintf(stderr, "native: cannot reserve guest address space\n"); return 1; }
    if (load_dump(dump)) return 1;
    fps = g_fps = set_frame_rate(fps);
    video_set_fps(fps);
    msr_apply_patches();
    if (!exists(module)) { fprintf(stderr, "native: no recompiled code at %s: build it in the launcher (Native code)\n", module); return 1; }
#ifdef __ANDROID__
    module = msr_android_module(module);   /* to the app's private folder, where Android loads code from */
#endif
    if (rt_module_load(module)) return 1;
    msr_patches_module_loaded();
    if (g_code_orig) {   /* a game mod changed the code: the functions it changed run on the interpreter */
        rt_code_changed(g_code_base, g_code_orig, g_code_orig_size, g_code_mod, g_code_mod_size);
        free(g_code_orig); free(g_code_mod); g_code_orig = g_code_mod = NULL;
    }
    msr_textures_start();
    { extern void gsp_hook_enqueue(void); gsp_hook_enqueue(); }   /* command lists copied as the game queues them (gsp.c) */
    k_init();
    dsp_set_volume(g_mute ? 0.f : (float)volume / 100.f);
    msr_overlay_start(video_fullscreen(), video_resolution(), video_vsync(), g_mute ? 0 : volume);
    if (video_init(scale)) { fprintf(stderr, "native: video init failed\n"); return 1; }
    fprintf(stderr, "native: boot (input=%s, audio=%s)\n", g_input_none ? "none" : "default", g_mute ? "muted" : "on");

    volatile int *done = k_start_main(0x100000);
    double next = now_s(), last_stat = next; unsigned frames = 0;
    while (!*done && g_exit < 0) {
        {   /* PWR_HITCH (gsp.c): the longest pass of this loop between two game frames */
            extern double g_main_iter_max_ms; static double prev; double t = now_s();
            if (prev > 0 && (t - prev) * 1e3 > g_main_iter_max_ms) g_main_iter_max_ms = (t - prev) * 1e3;
            prev = t;
        }
        fps = g_fps;
        extern double g_main_part_max[5]; double pt0 = now_s(), pt1, pt2, pt3, pt4;   /* PWR_HITCH: each part's longest */
        input_update(); pt1 = now_s();
        gsp_vblank(); pt2 = now_s();
        msr_cheats_frame(now_s());   /* Samus Returns' cheats and the extras' patches (msr/cheats.cpp) */
        msr_camera_frame(now_s());   /* the camera zoom: the game's field of view widened for it (msr/camera_fov.cpp) */
        pt3 = now_s();
        if (!video_frame()) break;
        pt4 = now_s();
        { double d[4] = { pt1 - pt0, pt2 - pt1, pt3 - pt2, pt4 - pt3 }; for (int i = 0; i < 4; i++) if (d[i] * 1e3 > g_main_part_max[i]) g_main_part_max[i] = d[i] * 1e3; }
        frames++;
        next = next_tick(next, fps);   /* VBlank at the frame rate above 60 (the game waits for it); the display's when that is the rate */
        double t = now_s();
        if (next > t) k_sleep_ns((int64_t)((next - t) * 1e9));   /* to the microsecond: the game's frame limiter wakes just before it */
        else if (t - next > 0.25) next = t;          /* fell behind: don't try to catch up */
        if (t - last_stat >= 2.0) {
            uint32_t lists, draws; pica_stats(&lists, &draws);
            extern unsigned g_vid_acquire_fail, g_vid_present_fail, g_vid_presented; extern int g_vid_last_err;
            extern unsigned g_gsp_top_swaps; static unsigned last_swaps;
            extern double g_frame_max_ms; extern unsigned g_frame_over[3];
            fprintf(stderr, "native: frames=%u gpu-lists=%u draws=%u presented=%u acquire-fail=%u present-fail=%u last-err=%d game-fps=%.1f "
                    "frame-max=%.1fms over10=%u over20=%u over50=%u\n",
                    frames, lists, draws, g_vid_presented, g_vid_acquire_fail, g_vid_present_fail, g_vid_last_err, (g_gsp_top_swaps - last_swaps) / (t - last_stat),
                    g_frame_max_ms, g_frame_over[0], g_frame_over[1], g_frame_over[2]);
            g_frame_max_ms = 0; memset(g_frame_over, 0, sizeof g_frame_over);
            {   /* no new top-screen frame for 6 s after the game had started drawing: say where everything waits, once */
                static unsigned still; static int reported;
                if (g_gsp_top_swaps == last_swaps && g_gsp_top_swaps) still++; else still = 0;
                if (still == 3 && !reported) { reported = 1; fprintf(stderr, "hang: no game frame for 6 s\n"); k_hang_report(); gsp_hang_report(); }
                static double t0; if (!t0) t0 = t;   /* PWR_HANG_AT=<s>: the same report at that run time, frames or not */
                if (getenv("PWR_HANG_AT") && !reported && t - t0 >= atof(getenv("PWR_HANG_AT"))) {
                    reported = 1; fprintf(stderr, "hang: report at %.0f s (PWR_HANG_AT)\n", t - t0); k_hang_report(); gsp_hang_report();
                }
            }
            last_swaps = g_gsp_top_swaps;
            { extern void dsp_stats(void); dsp_stats(); extern void gsp_stats(void); gsp_stats(); }
            if (msr_textures_active()) { unsigned nt, nl, nf; msr_textures_stats(&nt, &nl, &nf);
                                         fprintf(stderr, "msr: texture pack: %u loaded of %u, %u unreadable\n", nl, nt, nf); }
            if (getenv("PWR_PEEK")) {   /* PWR_PEEK=<hex guest addr>[,<words>]: print guest memory every stats interval */
                unsigned pa = 0, pn = 8; sscanf(getenv("PWR_PEEK"), "%x,%u", &pa, &pn); fprintf(stderr, "peek %08x:", pa);
                for (unsigned i = 0; i < pn && i < 512; i++) fprintf(stderr, " %08x", rd32(pa + 4 * i));
                fprintf(stderr, "\n");
            }
            extern unsigned long long g_sw_tri_in, g_sw_tri_drawn, g_sw_px_cov, g_sw_px_zfail, g_sw_px_afail, g_sw_px_written;
            ScreenSource st, sb; gsp_screens(&st, &sb);
            extern unsigned long long g_sw_clip_plane[7];
            fprintf(stderr, "native: clipped by plane x<w %llu x>-w %llu y<w %llu y>-w %llu z<0 %llu z>-w %llu w>0 %llu\n", g_sw_clip_plane[0], g_sw_clip_plane[1], g_sw_clip_plane[2], g_sw_clip_plane[3], g_sw_clip_plane[4], g_sw_clip_plane[5], g_sw_clip_plane[6]);
            fprintf(stderr, "native: sw tris=%llu drawn=%llu px covered=%llu zfail=%llu afail=%llu written=%llu | top %08x fmt%u bottom %08x fmt%u\n",
                    g_sw_tri_in, g_sw_tri_drawn, g_sw_px_cov, g_sw_px_zfail, g_sw_px_afail, g_sw_px_written, st.addr, st.format, sb.addr, sb.format);
            extern unsigned long long g_vk_draws, g_vk_batches, g_vk_uploads, g_vk_tex_uploads, g_vk_passes, g_vk_gpu_us, g_vk_wait_us, g_vk_flush_us, g_vk_verts, g_vk_idx;
            extern unsigned g_vsgen_compiled, g_vsgen_failed;
            extern int pica_vk_npipes(void);
            fprintf(stderr, "native: vk draws=%llu batches=%llu target-uploads=%llu tex-uploads=%llu vs-compiled=%u vs-failed=%u passes=%llu gpu-ms=%llu fence-wait-ms=%llu verts=%llu idx=%llu pipelines=%d\n", g_vk_draws, g_vk_batches, g_vk_uploads, g_vk_tex_uploads, g_vsgen_compiled, g_vsgen_failed, g_vk_passes, g_vk_gpu_us / 1000, g_vk_wait_us / 1000, g_vk_verts, g_vk_idx, pica_vk_npipes());
            { extern unsigned long long g_gs_draws; if (g_gs_draws) fprintf(stderr, "native: geometry-shader draws=%llu (not implemented)\n", g_gs_draws); }
            fflush(stderr);   /* the log, in one write (log_buffered) */
            last_stat = t;
        }
    }
    fprintf(stderr, "native: %s\n", *done ? "guest main returned" : g_exit == MSR_EXIT_RESTART ? "restart" : "window closed");
    { extern void pica_vk_saves_wait(void); pica_vk_saves_wait(); }   /* a shader cache save under way lands first */
    video_shutdown();
#ifndef _WIN32
    /* Linux and macOS: end here, without exit()'s static destructors. The guest threads, the GX worker and the texture packs'
       PNG workers are still running, and glibc's pthread_cond_destroy waits for the waiters of a condition variable:
       the PNG workers' (msr/textures.cpp) never left, so the game never exited once its window closed or Restart was
       chosen (2026-11-03, Steam Deck: the launcher waited for it). Windows' runtime does not wait there. */
    { extern void dsp_close_wav(void); dsp_close_wav(); }   /* PWR_DSP_WAV's header (atexit) */
    fflush(NULL);
#ifdef __ANDROID__
    android_after_game(g_exit == MSR_EXIT_RESTART, *done || g_exit == 0);   /* the window closed: the app stays closed */
#endif
    _exit(g_exit > 0 ? g_exit : 0);
#endif
    return g_exit > 0 ? g_exit : 0;
}
