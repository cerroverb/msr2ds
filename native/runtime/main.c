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
#ifndef _WIN32
#define _GNU_SOURCE   /* glibc's dladdr, Dl_info and REG_RIP (the crash report, crash_report) */
#endif
#include "gpu.h"
#include "runtime.h"
#include "dump.h"
#include "input.h"
#include "msr/msr.h"
#include <SDL_filesystem.h>
#include <math.h>
#include <pthread.h>
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
#include <sys/resource.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <unwind.h>   /* the crash report's calls (crash_report) */
#ifdef __linux__
#include <sys/syscall.h>
#include <ucontext.h>
#endif
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
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
   them through interp.c. Returns the rate in effect.
   Power-saving waits (main_set_power_saving): above 30 FPS the limit is a millisecond shorter (15 ms at 60), so the
   limiter, waking on the timer alone, still has the time to the VBlank to spare; the VBlank wait sets the pace as before.
   At 30 FPS the limit itself sets the pace (the VBlank after it was signalled during the frame, so the wait returns at
   once: 33 ms gives 30.3 FPS, 32 ms gave 31.2, measured 2026-11-05), and a late wake-up only lengthens that one frame. */
static int power_saving;
static int set_frame_rate(int fps) {
    static int applied;   /* 1 once the words were checked and changed: then they may be changed again, or put back */
    uint32_t limit = fps > 30 ? (uint32_t)((1000 + fps - 1) / fps - 1) : 33u;
    if (power_saving && fps > 30) limit--;
    const int limit_patched = limit != 33u, steps_patched = fps > 30;   /* the steps take the measured time above 30 FPS */
    const struct { uint32_t addr, original, replacement; int on; } patches[] = {
        { 0x156484, 0xE3500021, 0xE3500000 | limit, limit_patched },   /* cmp r0, #33 -> #limit */
        { 0x15648C, 0xE2606021, 0xE2606000 | limit, limit_patched },   /* rsb r6, r0, #33 -> #limit */
        { 0x49DFF8, 0xED9F8AA6, 0xEEB08A40, steps_patched },           /* the HUD: vldr s16, =1/30 -> vmov.f32 s16, s0 */
        { 0x4E3BA4, 0xED9F8AD5, 0xE1A00000, steps_patched },           /* an interface class: vldr s16, =1/30 -> mov r0, r0 */
        { 0x10C6B4, 0x1D940AD0, 0xE1A00000, steps_patched },           /* the interface's fixed-step mode: vldrne s0, [r4, #0x340] -> nop */
    };
    if (!limit_patched && !steps_patched && !applied) return 30;
    if (!applied) {
        for (size_t i = 0; i < sizeof patches / sizeof *patches; i++)
            if (rd32(patches[i].addr) != patches[i].original) {
                fprintf(stderr, "native: the frame-rate patch does not fit this program (%08x): staying at 30 FPS\n", patches[i].addr);
                return 30;
            }
        applied = 1;
    }
    for (size_t i = 0; i < sizeof patches / sizeof *patches; i++) wr32(patches[i].addr, patches[i].on ? patches[i].replacement : patches[i].original);
    fprintf(stderr, "native: %d FPS (frame limit %u ms%s)\n", fps > 30 ? fps : 30, limit, power_saving ? ", power-saving waits" : "");
    return fps > 30 ? fps : 30;
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
/* the next VBlank tick after one at `next`: the frame period on the host clock, or the display's next vblank less 2 ms.
   The display is followed when the frame period is a whole number of its refresh periods (60 FPS on a 120 or 240 Hz
   display too: on the host clock that drifted across a vblank every few seconds, frames then 1 or 3 vblanks apart, a
   judder that came and went, #49), and when DWM's clock is the window's display's: DWM times the desktop's composition,
   which with several monitors (or VRR) need not be the one the game is on (its refresh from SDL, video.c). */
double video_display_hz(void);
static double next_tick(double next, int fps) {
    double step = fps > 60 ? 1.0 / fps : 1.0 / 60.0;
    static int off = -1; if (off < 0) off = getenv("PWR_NO_DISPLAY_SYNC") != NULL;
    static double period, vblank, checked; static int ok, logged;
    double t = now_s();
    if (!off && t - checked > 0.5) {
        checked = t; ok = display_timing(&period, &vblank);
        const double hz = video_display_hz();
        if (ok && hz > 0 && fabs(1.0 / period - hz) > 1.5 + hz * 0.01) ok = 0;   /* another display's clock */
    }
    const double n = ok ? floor(step / period + 0.5) : 0;   /* vblanks a frame */
    if (off || !ok || n < 1 || fabs(n * period - step) > step * 0.015) return next + step;
    if (!logged) { logged = 1; fprintf(stderr, "native: VBlank follows the display (%.3f Hz%s)\n", 1.0 / period, n > 1 ? ", every other vblank or more" : ""); }
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
/* Power-saving waits: the GX worker (gsp.c) and every precise sleep (kernel.c k_sleep_ns) wait without spinning, and the
   frame limit leaves a millisecond more before the VBlank (set_frame_rate). By default with 4 processor threads or fewer. */
static int power_saving_default(void) { return k_cpu_threads() <= 4; }
void main_set_power_saving(int on) {
    power_saving = on != 0;
    k_set_power_saving(power_saving);
    g_fps = set_frame_rate(g_fps);
}
void main_exit(int code) { g_exit = code; }

/* the folder holding a file ("a/b/c.ini" -> "a/b/"), "" for none */
static void folder_of(const char *path, char *out, size_t n) {
    const char *slash = strrchr(path, '/'), *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    snprintf(out, n, "%.*s", slash ? (int)(slash - path + 1) : 0, path);
}

/* ---- what the main thread and the GX thread are doing (gpu.h), and the watchdog ----
   A stage held for 5 s is logged with the other thread's, the game's threads' waits and the log flushed; again at 60 s.
   #52 (Mali-G57): the log stopped a few seconds into the game while the audio played on, the main thread stuck
   somewhere, and the report of a game without frames runs on that thread; #58 (Adreno 830): the launcher's log ended
   inside vk_open, which says nothing of where. */
static struct { const char *volatile what; volatile double since; } stages[STAGE_COUNT];
static const char *const stage_name[STAGE_COUNT] = { "the main thread", "the GX thread" };
static volatile int g_booted;   /* the game's threads exist: the watchdog's report includes where they wait */
const char *stage_set(int who, const char *what) {
    const char *was = stages[who].what;
    stages[who].since = now_s(); stages[who].what = what;
    return was;
}
static void stages_print(void) {
    const double t = now_s();
    for (int i = 0; i < STAGE_COUNT; i++) {
        const char *w = stages[i].what;
        fprintf(stderr, "watchdog:   %s: %s", stage_name[i], w ? w : "nothing that waits");
        if (w) fprintf(stderr, " (for %.1f s)", t - stages[i].since);
        fputc('\n', stderr);
    }
}
static void *watchdog(void *arg) {
    (void)arg;
    double told[STAGE_COUNT] = { 0 }; int times[STAGE_COUNT] = { 0 };
    for (;;) {
#ifdef _WIN32
        Sleep(500);
#else
        usleep(500000);
#endif
        const double t = now_s();
        for (int i = 0; i < STAGE_COUNT; i++) {
            const char *w = stages[i].what; const double since = stages[i].since;
            if (!w) continue;
            if (since != told[i]) times[i] = 0;
            if (t - since < (times[i] ? 60.0 : 5.0) || times[i] >= 2) continue;
            told[i] = since; times[i]++;
            fprintf(stderr, "watchdog: %s has been %s for %.0f s%s\n", stage_name[i], w, t - since, times[i] > 1 ? " (still)" : "");
            stages_print();
            fflush(stderr);   /* (before the reports: they read the game's state as it is) */
            if (g_booted && times[i] == 1) { k_hang_report(); gsp_hang_report(); }
            fflush(stderr);
        }
    }
    return NULL;
}
static void watchdog_start(void) {
    pthread_t th;
    if (!pthread_create(&th, NULL, watchdog, NULL)) pthread_detach(th);
}

/* The log (stderr, a file) is buffered and written every 2 s with the stats: unbuffered, each line cost milliseconds of
   writes (about 19 ms for the 2 s stats) on the thread that runs VBlank, a 26 ms frame and a catch-up frame every 2 s
   at 144 FPS (2026-11-02). It still reaches the file on exit, abort() and a crash. */
/* Windows: main has returned and the C runtime's exit is under way. Its static destructors run while the game's threads
   still execute (Linux and Android leave with _exit instead, main): one of them can then fault in the recompiled code's
   lookup (dispatch.cpp, its tables gone), which the log does not report, as before the crash report existed. */
#ifdef _WIN32
static volatile int g_teardown;
static void log_flush_on_abort(int sig) { (void)sig; fprintf(stderr, "native: aborted\n"); stages_print(); fflush(stderr); }
static LONG WINAPI log_flush_on_crash(EXCEPTION_POINTERS *e) {
    if (g_teardown) { fflush(stderr); return EXCEPTION_CONTINUE_SEARCH; }
    void *at = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionAddress : NULL;
    HMODULE m = NULL; char path[MAX_PATH] = "?";   /* where: module + offset (addr2line -e <module> <offset>) */
    if (at && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &m))
        GetModuleFileNameA(m, path, sizeof path);
    const char *base = strrchr(path, '\\'); base = base ? base + 1 : path;
    fprintf(stderr, "native: crashed: exception %08lx at %s+0x%llx, thread %lu\n", e && e->ExceptionRecord ? (unsigned long)e->ExceptionRecord->ExceptionCode : 0ul,
            base, m ? (unsigned long long)((uintptr_t)at - (uintptr_t)m) : (unsigned long long)(uintptr_t)at, (unsigned long)GetCurrentThreadId());
    stages_print(); fflush(stderr); return EXCEPTION_CONTINUE_SEARCH;
}
#else
/* A crash (Linux, Android, macOS): the log so far, then the signal, where it struck and the calls that led there (each
   as library+offset and the nearest exported name), before the system's own handling carries on (Android's tombstone,
   a core dump): its handler is put back and the faulting instruction runs again. Without this, a crash lost the log's
   last 2 s and said nothing of itself (#53: an Adreno 640 closing a few seconds in, the log ending at its first stats
   line). snprintf and dladdr are not async-signal-safe, but the process is ending anyway. */
static struct sigaction crash_old[NSIG];
static int crash_fd = 2;
static void crash_write(const char *s) { size_t n = strlen(s); while (n) { ssize_t w = write(crash_fd, s, n); if (w <= 0) break; s += w; n -= (size_t)w; } }
static void crash_where(const char *what, uintptr_t pc) {
    char line[384]; Dl_info di;
    if (pc && dladdr((void *)pc, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/'); base = base ? base + 1 : di.dli_fname;
        if (di.dli_sname && di.dli_saddr)
            snprintf(line, sizeof line, "native:   %s %s+0x%lx (%s+0x%lx)\n", what, base, (unsigned long)(pc - (uintptr_t)di.dli_fbase), di.dli_sname,
                     (unsigned long)(pc - (uintptr_t)di.dli_saddr));
        else snprintf(line, sizeof line, "native:   %s %s+0x%lx\n", what, base, (unsigned long)(pc - (uintptr_t)di.dli_fbase));
    } else snprintf(line, sizeof line, "native:   %s 0x%lx\n", what, (unsigned long)pc);
    crash_write(line);
}
static _Unwind_Reason_Code crash_frame(struct _Unwind_Context *c, void *arg) {
    int *n = arg; const uintptr_t pc = (uintptr_t)_Unwind_GetIP(c);
    if (!pc || ++*n > 32) return _URC_END_OF_STACK;
    char what[16]; snprintf(what, sizeof what, "#%d", *n - 1); crash_where(what, pc);
    return _URC_NO_REASON;
}
static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    static volatile sig_atomic_t busy;
    if (!busy) {   /* the first thread to crash reports */
        busy = 1;
        fflush(stderr);   /* the lines since the last stats */
        crash_fd = fileno(stderr);
        const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : sig == SIGABRT ? "SIGABRT" : "?";
        char line[256];
#ifdef __linux__
        const long tid = (long)syscall(SYS_gettid);
        snprintf(line, sizeof line, "native: crashed: %s (code %d) at address %p, thread %ld%s\n", name, si->si_code, si->si_addr, tid, tid == (long)getpid() ? " (the main thread)" : "");
#else
        snprintf(line, sizeof line, "native: crashed: %s (code %d) at address %p\n", name, si->si_code, si->si_addr);
#endif
        crash_write(line);
        for (int i = 0; i < STAGE_COUNT; i++) {   /* what each was doing (constant strings) */
            const char *w = stages[i].what;
            crash_write("native:   "); crash_write(stage_name[i]); crash_write(w ? " was " : " was not waiting on anything marked"); if (w) crash_write(w); crash_write("\n");
        }
#if defined(__linux__) && defined(__aarch64__)
        const ucontext_t *uc = ctx;
        crash_where("pc", (uintptr_t)uc->uc_mcontext.pc); crash_where("lr", (uintptr_t)uc->uc_mcontext.regs[30]);
#elif defined(__linux__) && defined(__x86_64__) && defined(REG_RIP)
        const ucontext_t *uc = ctx;
        crash_where("pc", (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]);
#else
        (void)ctx;
#endif
        crash_write("native:   called from (the handler's own frames first):\n");
        int n = 0; _Unwind_Backtrace(crash_frame, &n);
    }
    sigaction(sig, &crash_old[sig], NULL);   /* the system's handling next */
    if (si->si_code <= 0 || sig == SIGABRT) raise(sig);   /* sent, not a fault: it would not happen again by itself */
}
static void crash_report(void) {
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, &crash_old[sigs[i]]);
}
#endif
static void log_buffered(void) {
    watchdog_start();
#ifndef _WIN32
    crash_report();   /* with an unbuffered log too */
#else
    signal(SIGABRT, log_flush_on_abort);
    SetUnhandledExceptionFilter(log_flush_on_crash);
#endif
    if (getenv("PWR_LOG_UNBUFFERED")) return;   /* every line at once, as before (debugging a hang without a stats line) */
    setvbuf(stderr, NULL, _IOFBF, 1 << 16);
}

int msr_launcher_main(void);
int msr_launcher_direct(void);

/* ---- what a slow computer is short of, in the log players send (2026-11-06) ---- */
/* the process's processor time, all its threads, in seconds */
static double process_cpu_s(void) {
#ifdef _WIN32
    FILETIME created, ended, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) return 0;
    ULARGE_INTEGER k, u; k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return (double)(k.QuadPart + u.QuadPart) * 1e-7;
#else
    struct rusage r; if (getrusage(RUSAGE_SELF, &r)) return 0;
    return (double)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1e-6;
#endif
}
static void cpu_name(char *out, size_t n) {
    out[0] = 0;
#ifdef _WIN32
    HKEY key; DWORD size = (DWORD)n - 1;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExA(key, "ProcessorNameString", NULL, NULL, (LPBYTE)out, &size) == ERROR_SUCCESS) out[size < n ? size : n - 1] = 0;
        else out[0] = 0;
        RegCloseKey(key);
    }
#elif defined(__APPLE__)
    size_t size = n; if (sysctlbyname("machdep.cpu.brand_string", out, &size, NULL, 0)) out[0] = 0;
#else
    FILE *f = fopen("/proc/cpuinfo", "r"); char line[256];
    while (f && fgets(line, sizeof line, f)) {   /* x86's "model name"; on ARM "Hardware", where there is one */
        char *c = strchr(line, ':');
        if (!c || (strncmp(line, "model name", 10) && strncmp(line, "Hardware", 8))) continue;
        for (c++; *c == ' ' || *c == '\t'; c++) {}
        c[strcspn(c, "\r\n")] = 0; snprintf(out, n, "%s", c); break;
    }
    if (f) fclose(f);
#endif
    size_t len = strlen(out); while (len && out[len - 1] == ' ') out[--len] = 0;   /* Windows pads it */
    if (!out[0]) snprintf(out, n, "an unknown processor");
}
static double memory_gb(void) {
#ifdef _WIN32
    MEMORYSTATUSEX m; memset(&m, 0, sizeof m); m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (double)m.ullTotalPhys / 1073741824.0 : 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? (double)pages * (double)size / 1073741824.0 : 0;
#endif
}
/* once, at boot: the settings that decide the load, and the computer (video.c has logged the graphics) */
static void settings_line(int fps) {
    char cpu[160]; cpu_name(cpu, sizeof cpu);
    const int res = video_resolution(), aniso = video_aniso();
    char res_text[48];
    if (res) snprintf(res_text, sizeof res_text, "%dx internal resolution", res); else snprintf(res_text, sizeof res_text, "the window's internal resolution");
    char aniso_text[24];
    if (aniso > 1) snprintf(aniso_text, sizeof aniso_text, "%dx", aniso); else snprintf(aniso_text, sizeof aniso_text, "off");
    fprintf(stderr, "native: running at %s, %d FPS, V-Sync %s, %s upscaling, anisotropic filtering %s, power-saving waits %s, %s; "
            "on %s, %d threads, %.1f GB of memory\n", res_text, fps, video_vsync() ? "on" : "off", video_filter_fsr() ? "FSR 1" : "linear",
            aniso_text, k_power_saving() ? "on" : "off", msr_settings_wide_culling() ? "the wider view drawn" : "only the 3DS's view drawn",
            cpu, k_cpu_threads(), memory_gb());
}
/* Every 2 s, per game frame: the game's frame thread at work (the frame less its time waiting or asleep: kernel.c), the
   GX commands' time (the worker's, and the small ones the game's thread runs), the GPU's drawing (the 3D batches'
   timestamps, not the upscaling and HUD after them), and the processor threads the program kept busy. With the frame
   rate under the one asked for, the likeliest shortage. */
static void load_line(double seconds, unsigned game_frames, double frame_max_ms, int target) {
    extern volatile uint64_t g_k_frame_idle_ns; extern int g_k_frame_thread; extern unsigned long long g_gx_busy_ns, g_vk_gpu_us;
    static uint64_t idle0; static unsigned long long gx0, gpu0; static double cpu0 = -1;
    const double cpu = process_cpu_s();
    const uint64_t idle = g_k_frame_idle_ns; const unsigned long long gx = g_gx_busy_ns, gpu = g_vk_gpu_us;
    const int first = cpu0 < 0;
    const double threads_busy = (cpu - cpu0) / seconds, idle_s = (double)(idle - idle0) / 1e9;
    const double gx_s = (double)(gx - gx0) / 1e9, gpu_s = (double)(gpu - gpu0) / 1e6;
    cpu0 = cpu; idle0 = idle; gx0 = gx; gpu0 = gpu;
    if (first || game_frames < 2 || seconds <= 0) return;   /* the first interval, or loading: nothing to say */
    const double n = game_frames, fps = n / seconds, frame_ms = 1000.0 / fps;
    const double work_ms = g_k_frame_thread >= 0 && idle_s <= seconds ? (seconds - idle_s) * 1e3 / n : -1;
    const double gx_ms = gx_s * 1e3 / n, gpu_ms = gpu_s * 1e3 / n;
    const int threads = k_cpu_threads();
    const char *limit = "";
    if (fps < target * 0.95)
        limit = gpu_ms >= frame_ms * 0.8 ? "; likely short of: the graphics"
              : gx_ms >= frame_ms * 0.8 ? "; likely short of: one processor thread (GX commands)"
              : work_ms >= frame_ms * 0.8 ? "; likely short of: the processor (the game's own work)"
              : threads_busy >= threads * 0.85 ? "; likely short of: the processor (every thread busy)"
              : "";
    char work[32];
    if (work_ms >= 0) snprintf(work, sizeof work, "%.1f ms", work_ms); else snprintf(work, sizeof work, "?");
    fprintf(stderr, "native: load: %.1f of %d FPS (%.1f ms a frame, longest %.1f); per frame: the game's thread at work %s, GX commands "
            "%.1f ms, GPU drawing %.1f ms; processor %.1f of %d threads busy%s\n", fps, target, frame_ms, frame_max_ms, work, gx_ms, gpu_ms,
            threads_busy, threads, limit);
}

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
        video_keep_crashed_log("user/msr-native.log");   /* (a start that ended while opening the graphics: video.c) */
        if (freopen("user/msr-native.log", "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);   /* the launcher's is its own */
        /* user/env.txt: test switches (PWR_..., one NAME=value a line, # comments), as a PC's environment would give them;
           a phone has no other way to set them */
        FILE *ef = fopen("user/env.txt", "r"); char line[512];
        while (ef && fgets(line, sizeof line, ef)) {
            char *e = line + strcspn(line, "\r\n"); *e = 0;
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq || eq == line) continue;
            *eq = 0; setenv(line, eq + 1, 1);
            fprintf(stderr, "native: user/env.txt: %s=%s\n", line, eq + 1);
        }
        if (ef) fclose(ef);
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
    int scale = 2, fps = 0, volume = 100, packs_given = 0, saving = -1;
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
        else if (!strncmp(argv[i], "--power-saving=", 15)) saving = !strcmp(argv[i] + 15, "on") || !strcmp(argv[i] + 15, "1");
        else if (!strncmp(argv[i], "--", 2) && strchr(argv[i], '=')) {   /* display options: --name=value -> video_option */
            char name[32]; const char *eq = strchr(argv[i], '='); size_t n = (size_t)(eq - argv[i] - 2);
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, argv[i] + 2, n); name[n] = 0; for (char *c = name; *c; c++) if (*c == '-') *c = '_';
            if (video_option(name, eq + 1)) { fprintf(stderr, "native: unknown option or value %s\n", argv[i]); return 2; }
        }
        else { fprintf(stderr, "native: unknown option %s\n", argv[i]); return 2; }
    }
    power_saving = saving < 0 ? power_saving_default() : saving;   /* applied to the frame limit by set_frame_rate below */
    k_set_power_saving(power_saving);
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
    { extern uint32_t g_k_limiter_lr; g_k_limiter_lr = 0x156498; }   /* the frame limiter's svcSleepThread returns there (the load line) */
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
    msr_overlay_start(video_fullscreen(), video_resolution(), video_vsync(), g_mute ? 0 : volume, video_aniso(), video_filter_fsr(),
                      power_saving);
    if (video_init(scale)) { fprintf(stderr, "native: video init failed\n"); video_explain("user/msr-native.log"); return 1; }
    settings_line(fps);
    fprintf(stderr, "native: boot (input=%s, audio=%s)\n", g_input_none ? "none" : "default", g_mute ? "muted" : "on");

    volatile int *done = k_start_main(0x100000);
    g_booted = 1;
    double next = now_s(), last_stat = next; unsigned frames = 0;
    while (!*done && g_exit < 0) {
        {   /* PWR_HITCH (gsp.c): the longest pass of this loop between two game frames */
            extern double g_main_iter_max_ms; static double prev; double t = now_s();
            if (prev > 0 && (t - prev) * 1e3 > g_main_iter_max_ms) g_main_iter_max_ms = (t - prev) * 1e3;
            prev = t;
        }
        fps = g_fps;
        extern double g_main_part_max[5]; double pt0 = now_s(), pt1, pt2, pt3, pt4;   /* PWR_HITCH: each part's longest */
        stage_set(STAGE_MAIN, "reading input");
        input_update(); pt1 = now_s();
        stage_set(STAGE_MAIN, "giving the game its VBlank");
        {   /* 30 FPS: the game's VBlank on every other tick (30 Hz) while it keeps up. Its frame limit (33 ms) runs a little
               short of two 60 Hz VBlanks, so with each tick's VBlank it ran at 30.3 FPS and caught up with a 50 ms frame
               every 50 frames (1.65 s), a stutter; at 30 Hz each frame is two ticks, 30.0 FPS. A computer that cannot keep
               up (more than one late frame in half a second) gets every tick again, which let it run faster then (19.8
               FPS against 17.7 at 30 Hz, overloaded on one processor thread, 2026-11-06); after a second at 30 it goes
               back. PWR_VBLANK60=1: every tick always (A/B). */
            extern unsigned g_gsp_top_swaps;
            static int off = -1, keeping_up = 1, good, told; static unsigned tick, window, swaps;
            if (off < 0) off = getenv("PWR_VBLANK60") != NULL;
            int deliver = 1;
            if (fps <= 30 && !off) {
                if (++window >= 30) {   /* 0.5 s: 15 game frames when it keeps up */
                    const unsigned n = g_gsp_top_swaps - swaps; swaps = g_gsp_top_swaps; window = 0;
                    if (keeping_up && n <= 13) {
                        keeping_up = 0; good = 0;
                        if (told++ < 20) fprintf(stderr, "native: 30 FPS behind (%u frames in 0.5 s): VBlank each tick\n", n);
                    } else if (!keeping_up) {
                        good = n >= 15 ? good + 1 : 0;
                        if (good >= 2) { keeping_up = 1; if (told < 20) fprintf(stderr, "native: 30 FPS kept up: VBlank every other tick\n"); }
                    }
                }
                deliver = !keeping_up || (tick++ & 1) == 0;
            } else { window = 0; swaps = g_gsp_top_swaps; keeping_up = 1; }
            if (deliver) gsp_vblank();
        }
        pt2 = now_s();
        stage_set(STAGE_MAIN, "applying the cheats and the camera");
        msr_cheats_frame(now_s());   /* Samus Returns' cheats and the extras' patches (msr/cheats.cpp) */
        msr_camera_frame(now_s());   /* the camera zoom: the game's field of view widened for it (msr/camera_fov.cpp) */
        pt3 = now_s();
        stage_set(STAGE_MAIN, "presenting (video_frame)");
        if (!video_frame()) break;
        stage_set(STAGE_MAIN, NULL);   /* (the wait for the next tick and the 2 s stats) */
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
            load_line(t - last_stat, g_gsp_top_swaps - last_swaps, g_frame_max_ms, fps);
            {   /* the main thread's waits in presenting, per presented frame (video.c) */
                extern double g_vid_wait_fence, g_vid_wait_acquire, g_vid_wait_present, g_vid_wait_queue_gx; static unsigned last_presented;
                const unsigned np = g_vid_presented - last_presented; last_presented = g_vid_presented;
                if (np) fprintf(stderr, "native: presenting waits per frame: last frame's fence %.2f ms, next image %.2f ms, present %.2f ms; "
                                "the GX thread's wait for the queue %.2f ms\n", g_vid_wait_fence * 1e3 / np, g_vid_wait_acquire * 1e3 / np,
                                g_vid_wait_present * 1e3 / np, g_vid_wait_queue_gx * 1e3 / np);
                g_vid_wait_fence = g_vid_wait_acquire = g_vid_wait_present = g_vid_wait_queue_gx = 0;
            }
            g_frame_max_ms = 0; memset(g_frame_over, 0, sizeof g_frame_over);
            {   /* no new top-screen frame for 6 s after the game had started drawing: say where everything waits, once */
                static unsigned still; static int reported;
                if (g_gsp_top_swaps == last_swaps && g_gsp_top_swaps) still++; else still = 0;
                if (still == 3 && !reported) { reported = 1; fprintf(stderr, "hang: no game frame for 6 s\n"); k_hang_report(); gsp_hang_report(); }
                static double t0; if (!t0) t0 = t;   /* PWR_HANG_AT=<s>: the same report at that run time, frames or not */
                if (getenv("PWR_HANG_AT") && !reported && t - t0 >= atof(getenv("PWR_HANG_AT"))) {
                    reported = 1; fprintf(stderr, "hang: report at %.0f s (PWR_HANG_AT)\n", t - t0); k_hang_report(); gsp_hang_report();
                }
                static int quick_loaded;   /* PWR_QUICK_LOAD_AT=<s>: the quick state loaded at that run time (F8 without a keyboard: tests) */
                if (getenv("PWR_QUICK_LOAD_AT") && !quick_loaded && t - t0 >= atof(getenv("PWR_QUICK_LOAD_AT"))) {
                    extern void msr_state_quick_load(void); quick_loaded = 1; msr_state_quick_load();
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
            extern unsigned long long g_vk_gpu_depth_us, g_vk_gpu_flat_us, g_vk_passes_flat;
            extern unsigned g_vsgen_compiled, g_vsgen_failed;
            extern int pica_vk_npipes(void);
            fprintf(stderr, "native: vk draws=%llu batches=%llu target-uploads=%llu tex-uploads=%llu vs-compiled=%u vs-failed=%u passes=%llu (%llu without depth) gpu-ms=%llu (passes with depth %llu, without %llu) fence-wait-ms=%llu verts=%llu idx=%llu pipelines=%d\n", g_vk_draws, g_vk_batches, g_vk_uploads, g_vk_tex_uploads, g_vsgen_compiled, g_vsgen_failed, g_vk_passes, g_vk_passes_flat, g_vk_gpu_us / 1000, g_vk_gpu_depth_us / 1000, g_vk_gpu_flat_us / 1000, g_vk_wait_us / 1000, g_vk_verts, g_vk_idx, pica_vk_npipes());
            { extern unsigned long long g_gs_draws; if (g_gs_draws) fprintf(stderr, "native: geometry-shader draws=%llu (not implemented)\n", g_gs_draws); }
            fflush(stderr);   /* the log, in one write (log_buffered) */
            last_stat = t;
        }
    }
    stage_set(STAGE_MAIN, "closing the game");
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
#ifdef _WIN32
    g_teardown = 1;   /* (log_flush_on_crash) */
#endif
    return g_exit > 0 ? g_exit : 0;
}
