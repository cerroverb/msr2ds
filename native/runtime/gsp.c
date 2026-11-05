/* gsp::Gpu: shared memory (interrupt queues, framebuffer info, GX command queues), GX commands, VBlank.
 * Shared memory layout (0x1000 bytes):
 *   +0x000 + 0x40*thread  interrupt queue {u8 first, u8 count, u8 missed, u8 flags, u32 pad[3]?, u8 ids[0x34]} (ids at +0xC)
 *   +0x200 + 0x80*thread  top screen framebuffer info {u8 index, u8 update, u16 pad, FrameBufferInfo[2] (0x1C each)}
 *   +0x240 + 0x80*thread  bottom screen framebuffer info
 *   +0x800 + 0x200*thread GX command queue {u8 index, u8 count, u8 status, u8 stop, u32 pad[7], cmd[15] x 0x20}
 * GX command addresses are process virtual addresses.
 * GX commands run on a GPU worker thread (as the real GPU runs beside the CPU): TriggerCmdReqQueue only wakes it.
 * It takes one command at a time out of a queue under G, executes it without G (a software draw can take a long
 * time and must not stall the host VBlank tick), then raises the completion interrupt. */
#include "pica_int.h"
#include "state.h"
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int k_current_index(void);

#define HDR(id, normal, translate) (((uint32_t)(id) << 16) | ((normal) << 6) | (translate))
#define MAX_GSP_THREADS 4

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static KObject *shmem;                       /* the 0x1000-byte block; shmem->addr is where the game mapped it */
static KObject *events[MAX_GSP_THREADS];
static int nthreads;
static ScreenSource screens[2];
/* mirror of GPU MMIO written through WriteHWRegs (offsets from 0x1EB00000); 0x400400/0x400500 are the LCD blocks */
#define HWREG_SIZE 0x420000u
static uint32_t *hwregs;
static int screen_from_regs;   /* the game drives the LCDs through registers rather than SetBufferSwap */

static uint32_t hw(uint32_t off) { return hwregs && off < HWREG_SIZE ? hwregs[off / 4] : 0; }

/* LCD framebuffer block: +0x68/+0x6C left address A/B (physical), +0x70 format, +0x78 select, +0x90 stride */
static void screen_from_lcd_regs(int scr) {
    uint32_t b = scr ? 0x400500u : 0x400400u;
    uint32_t pa = (hw(b + 0x78) & 1) ? hw(b + 0x6C) : hw(b + 0x68);
    uint32_t va = pa_to_va(pa);
    if (!va) return;
    screens[scr].addr = va; screens[scr].format = hw(b + 0x70) & 7; screens[scr].stride = hw(b + 0x90); screens[scr].valid = 1;
}

unsigned g_gsp_top_swaps;   /* top-screen framebuffer swaps = game frames */
/* frame times between top-screen swaps, for main.c's 2 s line (stutter: the longest, how many over 10/20/50 ms) */
double g_frame_max_ms; unsigned g_frame_over[3];
double g_main_iter_max_ms, g_main_part_max[5];   /* main.c: its longest loop pass since the last frame, and of its parts (input, VBlank, cheats, video) (PWR_HITCH) */
static void frame_time_note(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6; static double last;
    /* PWR_HITCH=<ms>: each frame longer than that, with what the GX worker did meanwhile */
    static double hitch = -1; if (hitch < 0) { const char *h = getenv("PWR_HITCH"); hitch = h ? atof(h) : 0; }
    extern unsigned long long g_prof_list_ns, g_prof_fence_ns, g_prof_hash_ns, g_prof_wb_ns, g_prof_flush_ns, g_prof_draw_ns;
    static unsigned long long l0, f0, h0, w0, fl0, d0, y0[5]; static unsigned c0; extern unsigned g_gsp_gx_cmds;
    extern unsigned long long g_prof_wait_why[5];
    if (last > 0) {
        double d = now - last;
        if (d > g_frame_max_ms) g_frame_max_ms = d;
        g_frame_over[0] += d > 10; g_frame_over[1] += d > 20; g_frame_over[2] += d > 50;
        if (hitch > 0 && d > hitch) {
            #define DL(v, o) ((v) >= (o) ? ((v) - (o)) / 1e6 : (v) / 1e6)
            fprintf(stderr, "hitch: at %.0f ms frame %u took %.1f ms: GX lists %.1f (draws %.1f, flush %.1f), GPU waits %.1f, write-backs %.1f, hashing %.1f, "
                    "GX commands %u, main loop longest pass %.1f ms (input %.1f, VBlank %.1f, cheats %.1f, video %.1f); waits by cause: targets %.1f, GX commands %.1f, "
                    "set reuse %.1f + %.1f\n", now, g_gsp_top_swaps, d, DL(g_prof_list_ns, l0), DL(g_prof_draw_ns, d0),
                    DL(g_prof_flush_ns, fl0), DL(g_prof_fence_ns, f0), DL(g_prof_wb_ns, w0), DL(g_prof_hash_ns, h0), g_gsp_gx_cmds - c0, g_main_iter_max_ms,
                    g_main_part_max[0], g_main_part_max[1], g_main_part_max[2], g_main_part_max[3],
                    DL(g_prof_wait_why[0], y0[0]), DL(g_prof_wait_why[1], y0[1]), DL(g_prof_wait_why[2], y0[2]), DL(g_prof_wait_why[4], y0[4]));
            #undef DL
        }
    }
    l0 = g_prof_list_ns; f0 = g_prof_fence_ns; h0 = g_prof_hash_ns; w0 = g_prof_wb_ns; fl0 = g_prof_flush_ns; d0 = g_prof_draw_ns; c0 = g_gsp_gx_cmds;
    memcpy(y0, g_prof_wait_why, sizeof y0);
    g_main_iter_max_ms = 0; memset(g_main_part_max, 0, sizeof g_main_part_max);
    last = now;
}
static uint32_t sh(void) { return shmem ? shmem->addr : 0; }

/* The queue headers in shared memory (byte 0 first index, byte 1 count, byte 2 flag) are updated by the game's threads
   with LDREX/STREX while this runs on other host threads, so every header change here is one compare-and-swap of the
   word: separate byte writes could undo the game's own update in between (a lost GX command left the game waiting
   for a list that never ran, and a frozen screen). */
static uint32_t *hdr(uint32_t q) { return (uint32_t *)(g_mem + q); }

void k_ev(int code, uint32_t a, uint32_t b);
static void push_irq_now(int id) {
    k_ev(0x20, (uint32_t)id, 0);
    if (!shmem || !shmem->addr) return;
    for (int t = 0; t < nthreads; t++) {
        uint32_t q = sh() + 0x40u * (uint32_t)t, old = __atomic_load_n(hdr(q), __ATOMIC_SEQ_CST), nw;
        do {
            uint32_t first = old & 0xff, count = (old >> 8) & 0xff;
            if (count >= 0x34) nw = old | 0x10000u;                /* missed */
            else { wr8(q + 0xC + (first + count) % 0x34, (uint32_t)id); nw = (old & ~0xff00u) | (count + 1) << 8; }
        } while (!__atomic_compare_exchange_n(hdr(q), &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        /* signalled even when the queue is full: if the game's handler ever missed a wake-up, the queue fills (VBlanks
           alone do it in under half a second) and without a signal it would never run again */
        if (events[t]) k_signal(events[t]);
    }
}

/* While TriggerCmdReqQueue waits for small command lists to finish (PWR_GX_SYNC_MAX), the interrupts they raise are held back
   and delivered after the call returns, as the game expects (it learns of completion only after the trigger returned). */
static int irq_hold, nheld; static int held[64];
static void push_irq(int id) {
    if (irq_hold) {   /* one of each: a long hold must not overflow the game's 52-entry queue with VBlanks (that dropped the P3D) */
        for (int i = 0; i < nheld; i++) if (held[i] == id) return;
        if (nheld < 64) held[nheld++] = id;
        return;
    }
    push_irq_now(id);
}

/* framebuffer info for one screen: apply the pending entry if the update flag is set */
static void take_fb_info(int screen) {
    for (int t = 0; t < nthreads; t++) {
        uint32_t base = sh() + 0x200u + 0x80u * (uint32_t)t + (screen ? 0x40u : 0u);
        if (!rd8(base + 1)) continue;
        uint32_t idx = rd8(base) & 1, e = base + 4 + 0x1Cu * idx;
        /* FrameBufferInfo: active_fb, left vaddr, right vaddr, stride, format, select, attribute */
        screens[screen].addr = rd32(e + 4); screens[screen].stride = rd32(e + 12);
        screens[screen].format = rd32(e + 16) & 7; screens[screen].valid = 1;
        wr8(base + 1, 0);
        screen_from_regs = 0;   /* per-frame swaps through shared memory take over from the initial LCD registers */
        if (screen == 0) { g_gsp_top_swaps++; frame_time_note(); }
    }
}

static int gx_busy;   /* the worker holds a popped command (under G) */

/* PWR_GX_TIMELINE=1: one line per GX command start/end and per VBlank (pushed or held back), with host time */
static int gxt = -1;
uint64_t k_ticks(void);
static double gxt_now(void) { return k_ticks() / 268111856.0; }   /* same clock as the kernel's PWR_TRACE_SLEEP lines */
#define GXT(...) do { if (gxt < 0) gxt = getenv("PWR_GX_TIMELINE") != NULL; if (gxt) { fprintf(stderr, "gxt: %.4f ", gxt_now()); fprintf(stderr, __VA_ARGS__); } } while (0)

/* PWR_GX_STATS=1: the worker's time per GX command type and the VBlanks held back, printed with the 2 s stats */
static uint64_t gxs_ns[8], gxs_n[8], gxs_held, gxs_late, gxs_vb;
/* hand-off latencies: TriggerCmdReqQueue -> the worker starts it; the worker's completion interrupt -> the next trigger */
static uint64_t gxl_trig, gxl_end, gxl_ts, gxl_tsn, gxl_et, gxl_etn;
void gsp_stats(void) {
    static int on = -1; if (on < 0) on = getenv("PWR_GX_STATS") != NULL;
    if (!on) return;
    pthread_mutex_lock(&G);
    fprintf(stderr, "gx: vblanks %llu, held %llu (delivered late %llu);", (unsigned long long)gxs_vb, (unsigned long long)gxs_held, (unsigned long long)gxs_late);
    for (int i = 1; i < 6; i++) if (gxs_n[i]) fprintf(stderr, " %02x: %llu x %.2f ms", i, (unsigned long long)gxs_n[i], gxs_ns[i] / 1e6 / gxs_n[i]);
    fprintf(stderr, "; trigger->start %llu x %.3f ms, irq->trigger %llu x %.3f ms\n", (unsigned long long)gxl_tsn, gxl_tsn ? gxl_ts / 1e6 / gxl_tsn : 0.,
            (unsigned long long)gxl_etn, gxl_etn ? gxl_et / 1e6 / gxl_etn : 0.);
    gxl_ts = gxl_tsn = gxl_et = gxl_etn = 0;
    { extern unsigned long long g_prof_hash_ns, g_prof_wb_ns, g_prof_flush_ns, g_prof_draw_ns, g_prof_list_ns, g_prof_fence_ns, g_prof_tex_ns;
      extern unsigned g_gsp_top_swaps; static unsigned last; double fr = g_gsp_top_swaps - last ? (double)(g_gsp_top_swaps - last) : 1; last = g_gsp_top_swaps;
      extern unsigned long long g_vk_gpu_us; static unsigned long long gpu_last;
      fprintf(stderr, "gx: per frame: lists %.2f ms = draws %.2f (textures %.2f) + flush %.2f + rest; waits for the GPU %.2f, write-backs %.2f (waits included); hashing %.2f; GPU busy %.2f\n",
              g_prof_list_ns / 1e6 / fr, g_prof_draw_ns / 1e6 / fr, g_prof_tex_ns / 1e6 / fr, g_prof_flush_ns / 1e6 / fr, g_prof_fence_ns / 1e6 / fr,
              g_prof_wb_ns / 1e6 / fr, g_prof_hash_ns / 1e6 / fr, (g_vk_gpu_us - gpu_last) / 1e3 / fr);
      gpu_last = g_vk_gpu_us;
      { extern unsigned long long g_prof_wait_why[5];
        fprintf(stderr, "gx: GPU waits per frame: textures/targets %.2f, GX commands %.2f, set reuse %.2f + %.2f ms\n", g_prof_wait_why[0] / 1e6 / fr, g_prof_wait_why[1] / 1e6 / fr,
                g_prof_wait_why[2] / 1e6 / fr, g_prof_wait_why[4] / 1e6 / fr);
        memset(g_prof_wait_why, 0, sizeof g_prof_wait_why); }
      g_prof_hash_ns = g_prof_wb_ns = g_prof_flush_ns = g_prof_draw_ns = g_prof_list_ns = g_prof_fence_ns = g_prof_tex_ns = 0; }
    { extern void k_stats(void); pthread_mutex_unlock(&G); k_stats(); pthread_mutex_lock(&G); }
    memset(gxs_ns, 0, sizeof gxs_ns); memset(gxs_n, 0, sizeof gxs_n); gxs_held = gxs_late = gxs_vb = 0;
    pthread_mutex_unlock(&G);
}
static uint64_t gxs_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static int vblank_held;   /* a VBlank held back below: delivered once the worker has caught up (under G) */
static void deliver_vblank(void) {   /* under G */
    k_ev(0x21, 0, 0);
    vblank_held = 0;
    if (shmem && shmem->addr) { take_fb_info(0); take_fb_info(1); }
    push_irq(GSP_IRQ_VBLANK_TOP); push_irq(GSP_IRQ_VBLANK_BOTTOM);
}

void gsp_vblank(void) {
    {   /* PWR_TRACE_WINDOW=<start s>,<length s>: PWR_TRACE output only in that window (counted in 60 Hz VBlanks) */
        extern int g_trace; static long n, a = -2, b;
        if (a == -2) { const char *e = getenv("PWR_TRACE_WINDOW"); a = -1; if (e) { a = (long)(atof(e) * 60); const char *c = strchr(e, ',');
                       b = a + (long)((c ? atof(c + 1) : 1) * 60); } }
        if (a >= 0) { n++; if (n == a) { g_trace = 1; fprintf(stderr, "pwr: trace window opens\n"); } if (n == b) { g_trace = 0; fprintf(stderr, "pwr: trace window closes\n"); } }
    }
    pthread_mutex_lock(&G);
    {   /* The GPU of a 3DS keeps up with the game; the host worker may not, and the game then runs several frames ahead of
           it and reuses vertex/texture memory the worker has not read yet (garbled text and sky, wrong frames). Hold the
           tick back while more than PWR_GX_MAXLAG commands are queued or running (0: never), so the game paces to the GPU. */
        static int maxlag = -1; if (maxlag < 0) { const char *e = getenv("PWR_GX_MAXLAG"); maxlag = e ? atoi(e) : 1; }
        int lag = gx_busy;
        for (int q = 0; shmem && shmem->addr && q < nthreads; q++) lag += rd8(sh() + 0x801u + 0x200u * (uint32_t)q);
        gxs_vb++;
        if (maxlag > 0 && lag > maxlag) { GXT("vblank held (lag %d)\n", lag); vblank_held = 1; gxs_held++; pthread_mutex_unlock(&G); return; }
    }
    GXT("vblank\n");
    deliver_vblank();
    {   /* render targets' downloads still on their way land once a frame, while the worker is idle (the game's own code could
           read a target; on a 3DS the GPU has written it long before). PWR_VK_NO_VBLANK_WB=1: not here */
        static int off = -1; if (off < 0) off = getenv("PWR_VK_NO_VBLANK_WB") != NULL;
        if (!off && !gx_busy) pica_vk_sync_ready();   /* never waits for the GPU */
    }
    pthread_mutex_unlock(&G);
}

int gsp_screens(ScreenSource *top, ScreenSource *bottom) {
    pthread_mutex_lock(&G);
    if (screen_from_regs) { screen_from_lcd_regs(0); screen_from_lcd_regs(1); }
    *top = screens[0]; *bottom = screens[1]; pthread_mutex_unlock(&G);
    return top->valid || bottom->valid;
}

/* ---- GX commands ---- */
static const int bpp_of[8] = { 4, 3, 2, 2, 2, 0, 0, 0 };   /* RGBA8, RGB8, RGB565, RGB5A1, RGBA4 */

static void decode_px(const uint8_t *p, int fmt, uint8_t rgba[4]) {
    uint32_t v;
    switch (fmt) {
    case 0: rgba[0] = p[3]; rgba[1] = p[2]; rgba[2] = p[1]; rgba[3] = p[0]; return;
    case 1: rgba[0] = p[2]; rgba[1] = p[1]; rgba[2] = p[0]; rgba[3] = 255; return;
    case 2: v = p[0] | p[1] << 8; rgba[0] = (uint8_t)((v >> 11) * 255 / 31); rgba[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); rgba[2] = (uint8_t)((v & 31) * 255 / 31); rgba[3] = 255; return;
    case 3: v = p[0] | p[1] << 8; rgba[0] = (uint8_t)((v >> 11) * 255 / 31); rgba[1] = (uint8_t)(((v >> 6) & 31) * 255 / 31); rgba[2] = (uint8_t)(((v >> 1) & 31) * 255 / 31); rgba[3] = (v & 1) ? 255 : 0; return;
    case 4: v = p[0] | p[1] << 8; rgba[0] = (uint8_t)((v >> 12) * 17); rgba[1] = (uint8_t)(((v >> 8) & 15) * 17); rgba[2] = (uint8_t)(((v >> 4) & 15) * 17); rgba[3] = (uint8_t)((v & 15) * 17); return;
    default: memset(rgba, 0, 4);
    }
}
static void encode_px(uint8_t *p, int fmt, const uint8_t c[4]) {
    uint32_t v;
    switch (fmt) {
    case 0: p[0] = c[3]; p[1] = c[2]; p[2] = c[1]; p[3] = c[0]; return;
    case 1: p[0] = c[2]; p[1] = c[1]; p[2] = c[0]; return;
    case 2: v = (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 2) << 5 | (c[2] >> 3); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return;
    case 3: v = (uint32_t)(c[0] >> 3) << 11 | (uint32_t)(c[1] >> 3) << 6 | (uint32_t)(c[2] >> 3) << 1 | (c[3] >= 128); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return;
    case 4: v = (uint32_t)(c[0] >> 4) << 12 | (uint32_t)(c[1] >> 4) << 8 | (uint32_t)(c[2] >> 4) << 4 | (c[3] >> 4); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); return;
    }
}

/* offset of pixel (x, y) inside an 8x8-tiled (Morton) surface of the given width */
static uint32_t morton_offset(uint32_t x, uint32_t y, uint32_t width, uint32_t bpp) {
    static const uint8_t xl[8] = { 0, 1, 4, 5, 16, 17, 20, 21 }, yl[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
    uint32_t i = xl[x & 7] + yl[y & 7];
    return (i + (x & ~7u) * 8 + (y & ~7u) * width) * bpp;
}

static void memory_fill(uint32_t start, uint32_t value, uint32_t end, uint32_t control) {
    if (!start || end <= start) return;
    int width = (control >> 8) & 3;   /* 0: 16-bit, 1: 24-bit, 2/3: 32-bit */
    if (width == 0) { for (uint32_t a = start; a + 2 <= end; a += 2) wr16(a, value); }
    else if (width == 1) { for (uint32_t a = start; a + 3 <= end; a += 3) { wr8(a, value); wr8(a + 1, value >> 8); wr8(a + 2, value >> 16); } }
    else { uint8_t *p = g_mem + start; for (uint32_t n = (end - start) / 4; n--; p += 4) memcpy(p, &value, 4); }
}

static void display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags) {
    uint32_t iw = in_dim & 0xffff, ih = in_dim >> 16, ow = out_dim & 0xffff, oh = out_dim >> 16;
    int ifmt = (flags >> 8) & 7, ofmt = (flags >> 12) & 7, flip = flags & 1, out_tiled = (flags >> 1) & 1, raw = (flags >> 3) & 1;
    int scale = (flags >> 24) & 3, sx = scale ? 2 : 1, sy = scale == 2 ? 2 : 1;
    /* with scaling, the output size register holds the size before it (as Citra reads it): the image written is half as
       wide (and half as high for 2x2). Samus Returns' bloom and light-shaft chains downscale 256x512 -> 32x64 this way;
       taking the register as the real size gave the downscaled textures twice their tile-row stride (vertical bars). */
    ow /= (uint32_t)sx; oh /= (uint32_t)sy;
    /* bit 5: no swizzle conversion, both sides in the input's layout (bit 1 then means both linear, clear both tiled).
       Samus Returns downscales its tiled scene and depth copies tiled to tiled for bloom and depth of field. */
    int same = (flags >> 5) & 1, in_tiled = !out_tiled;
    if (same) out_tiled = in_tiled;
    int ib = bpp_of[ifmt], ob = bpp_of[ofmt];
    if (!ib || !ob || !iw || !ih) { fprintf(stderr, "gsp: DisplayTransfer with unsupported format %d->%d\n", ifmt, ofmt); return; }
    if (raw) { memcpy(g_mem + out, g_mem + in, (size_t)iw * ih * (size_t)ib); return; }
    if (same && in_tiled && scale == 2 && ifmt == 0 && ofmt == 0 && !flip && !(iw & 15) && !(ih & 15) && ow == iw / 2 && oh == ih / 2) {
        /* Samus Returns' bloom and light-shaft chains, every frame: tiled RGBA8 halved in both directions. In the tiled
           (Morton) order the 2x2 input block of an output pixel is 4 pixels in a row; the bytes are averaged as they are
           (truncated, as the generic loop does) */
        for (uint32_t y = 0; y < oh; y++) for (uint32_t x = 0; x < ow; x++) {
            const uint8_t *q = g_mem + in + morton_offset(x * 2, y * 2, iw, 4);
            uint8_t *d = g_mem + out + morton_offset(x, y, ow, 4);
            for (int k = 0; k < 4; k++) d[k] = (uint8_t)(((uint32_t)q[k] + q[4 + k] + q[8 + k] + q[12 + k]) / 4);
        }
        return;
    }
    if (!same && ifmt == 0 && ofmt == 1 && !out_tiled && !flip && !scale && iw == ow && oh <= ih && !(iw & 7) && !(oh & 7)) {
        /* the scan-out copy every frame: tiled RGBA8 -> linear RGB8 (the generic loop below costs ~25 ns per pixel, 10-20% of a
           heavy frame); also the bottom screen's, which takes the first 320 of 400 lines */
        static const uint8_t xl[8] = { 0, 1, 4, 5, 16, 17, 20, 21 }, yl[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
        uint32_t o[64]; for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) o[y * 8 + x] = (uint32_t)(xl[x] + yl[y]) * 4;
        for (uint32_t ty = 0; ty < oh / 8; ty++) for (uint32_t tx = 0; tx < iw / 8; tx++) {
            const uint8_t *t = g_mem + in + (ty * 8 * iw + tx * 64) * 4;
            uint8_t *d = g_mem + out + ((ty * 8) * ow + tx * 8) * 3;
            for (int y = 0; y < 8; y++, d += ow * 3) for (int x = 0; x < 8; x++) { const uint8_t *q = t + o[y * 8 + x]; d[x * 3] = q[1]; d[x * 3 + 1] = q[2]; d[x * 3 + 2] = q[3]; }
        }
        return;
    }
    for (uint32_t y = 0; y < oh; y++) for (uint32_t x = 0; x < ow; x++) {
        uint32_t acc[4] = { 0 }, n = 0;
        for (int dy = 0; dy < sy; dy++) for (int dx = 0; dx < sx; dx++) {
            uint32_t ix = x * (uint32_t)sx + (uint32_t)dx, iy = y * (uint32_t)sy + (uint32_t)dy;
            if (ix >= iw || iy >= ih) continue;
            if (flip) iy = ih - 1 - iy;
            uint32_t src = in_tiled ? morton_offset(ix, iy, iw, (uint32_t)ib) : (iy * iw + ix) * (uint32_t)ib;
            uint8_t c[4]; decode_px(g_mem + in + src, ifmt, c);
            for (int k = 0; k < 4; k++) acc[k] += c[k];
            n++;
        }
        if (!n) continue;
        uint8_t c[4]; for (int k = 0; k < 4; k++) c[k] = (uint8_t)(acc[k] / n);
        uint32_t dst = out_tiled ? morton_offset(x, y, ow, (uint32_t)ob) : (y * ow + x) * (uint32_t)ob;
        encode_px(g_mem + out + dst, ofmt, c);
    }
}

static void texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_wg, uint32_t out_wg) {
    uint32_t iw = (in_wg & 0xffff) * 16, ig = (in_wg >> 16) * 16, ow = (out_wg & 0xffff) * 16, og = (out_wg >> 16) * 16;
    if (!iw || !ow) { memcpy(g_mem + out, g_mem + in, size); return; }
    uint32_t done = 0, ip = in, op = out, irem = iw, orem = ow;
    while (done < size) {
        uint32_t n = irem < orem ? irem : orem; if (n > size - done) n = size - done;
        memcpy(g_mem + op, g_mem + ip, n); done += n; ip += n; op += n; irem -= n; orem -= n;
        if (!irem) { ip += ig; irem = iw; }
        if (!orem) { op += og; orem = ow; }
    }
}

/* Command lists are copied when the game queues them. The GPU worker runs a few GX commands behind the game, and the
 * game reuses a list buffer as soon as it has queued the list (the face library builds each Mii mask list at the start
 * of the same buffer): on hardware the GPU has read the list long before. The copy is taken by a hook on the game's GX
 * enqueue, kept for the queue slot the command goes to, and the worker runs the copy of the slot it takes the command
 * from. Samus Returns builds its little framebuffer set-up lists in a scratch buffer the same way, and run late at a higher
 * internal resolution they drew its bottom screen into the wrong target or not at all (a jittery file select): its enqueue
 * (nn::gxlow CmdReqQueueTx::TryEnqueue at 0x1AE170) is hooked through the dispatcher (gsp_hook_enqueue, rt_set_call_hook).
 * By slot, not in order of copying: when the queue is full the enqueue fails and the game calls it again, and a copy per
 * call put the top screen's set-up list in place of the bottom screen's (same scratch address and size) one frame in a few
 * at 60 FPS and most frames at 144 (the file select drawn 80 lines off). The slot is first + count of the queue header
 * when the enqueue starts: taking a command off the queue leaves that sum as it is. PWR_GX_NO_SNAPSHOT=1 runs lists from
 * guest memory instead. */
#define MAX_GSP_QUEUES 4
static struct { uint32_t va, size; uint8_t *data; } snaps[MAX_GSP_QUEUES][15];   /* under G */
static int snap_n;   /* copies held (gsp_hang_report) */
static pthread_cond_t gx_wake, gx_idle; static int gx_pending;   /* the worker's (below) */

void gsp_gx_enqueue_hook(CPU *c);
void gsp_gx_enqueue_hook(CPU *c) {   /* r0 -> pointer to the queue header, r1 = the 0x20-byte command */
    static int off = -1; if (off < 0) off = getenv("PWR_GX_NO_SNAPSHOT") != NULL;
    uint32_t q = rd32(c->r[0]), cmd = c->r[1];
    if (!q) return;
    /* The game never finds the queue full: a full queue fails the enqueue, and Samus Returns has two threads queueing GX
       commands (the main thread and the GSP interrupt thread). While one of them waits to try again, the other one's
       next command goes in first, and a command list ran after the copy that needed it, or not at all (the file select's
       bottom screen 80 lines off or empty; most frames at 144 FPS, where the game runs a frame ahead of the worker and
       keeps the queue near its 15 commands). On a 3DS the GPU empties the queue long before that. So the game waits here
       until the worker has taken a command, as it waits for the GPU anyway (at most ~0.2 s). */
    for (int rounds = 0; rd8(q + 1) >= 15 && rounds < 20; rounds++) {   /* checked again once this thread runs again */
        extern void k_wait_outside_begin(void), k_wait_outside_end(void);
        k_wait_outside_begin();   /* the game's other threads run meanwhile */
        pthread_mutex_lock(&G);
        for (int tries = 0; rd8(q + 1) >= 15 && tries < 10; tries++) {
            if (!gx_busy) { gx_pending = 1; pthread_cond_signal(&gx_wake); }
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 1000000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&gx_idle, &G, &ts);
        }
        pthread_mutex_unlock(&G);
        k_wait_outside_end();
    }
    if (off || rd8(cmd) != 0x01) return;
    uint32_t h = __atomic_load_n(hdr(q), __ATOMIC_SEQ_CST), first = h & 0xff, count = (h >> 8) & 0xff;
    if (count >= 15) return;   /* still full: the enqueue fails and the game calls it again */
    pthread_mutex_lock(&G);
    int t = shmem && shmem->addr && q >= sh() + 0x800u ? (int)((q - sh() - 0x800u) / 0x200u) : -1;
    pthread_mutex_unlock(&G);
    if (t < 0 || t >= MAX_GSP_QUEUES) return;
    uint32_t slot = (first + count) % 15, va = rd32(cmd + 4), size = rd32(cmd + 8);
    uint8_t *d = malloc(size ? size : 1); memcpy(d, g_mem + va, size);
    pthread_mutex_lock(&G);
    if (snaps[t][slot].data) { free(snaps[t][slot].data); snap_n--; }   /* an enqueue that failed, or a command never taken */
    snaps[t][slot].va = va; snaps[t][slot].size = size; snaps[t][slot].data = d; snap_n++;
    pthread_mutex_unlock(&G);
    /* PWR_GX_ENQ_LAG=<n>: hold the game here while more than n commands are queued or running, so it cannot rewrite the vertex
       and texture memory of the lists in flight (Pilotwings' face library builds many small lists back to back); at most
       ~100 ms. Off by default for Samus Returns (-1), whose lists only need the copy. */
    static int enq_lag = -2; if (enq_lag == -2) { const char *e = getenv("PWR_GX_ENQ_LAG"); enq_lag = e ? atoi(e) : -1; }
    /* PWR_GX_ENQ_MAX=<bytes>: only for lists up to that size (default 0x4000; ENQ_LAG default 0, -1 = off) */
    static uint32_t enq_max = 0xfffffffeu; if (enq_max == 0xfffffffeu) { const char *e = getenv("PWR_GX_ENQ_MAX"); enq_max = e ? (uint32_t)strtoul(e, NULL, 0) : 0x4000u; }
    for (int tries = 0; enq_lag >= 0 && size <= enq_max && tries < 400; tries++) {
        pthread_mutex_lock(&G);
        int lag = gx_busy;
        for (int t = 0; shmem && shmem->addr && t < nthreads; t++) lag += rd8(sh() + 0x801u + 0x200u * (uint32_t)t);
        pthread_mutex_unlock(&G);
        if (lag <= enq_lag) break;
        usleep(250);
    }
}

/* Samus Returns (USA): its GX enqueue, recognised by its first instructions (push {r4-r10, lr}; ldr r3, [r0]; cmp r3, #0;
   ...; ldrb r2, [r3, #1]; cmp r2, #15), gets the copy hook; anything else there (another version) is left alone */
void gsp_hook_enqueue(void) {
    static const uint32_t pc = 0x1AE170u;
    if (rd32(pc) != 0xE92D47F0u || rd32(pc + 4) != 0xE5903000u || rd32(pc + 8) != 0xE3530000u ||
        rd32(pc + 0x14) != 0xE5D32001u || rd32(pc + 0x18) != 0xE352000Fu) {
        fprintf(stderr, "gsp: the GX enqueue is not where Samus Returns (USA) has it: command lists run from guest memory\n");
        return;
    }
    rt_set_call_hook(pc, gsp_gx_enqueue_hook);
}

/* under G: the copy of list (va, size) taken for queue t's slot; NULL if there is none (or it is another list's) */
static uint8_t *take_snapshot(int t, int slot, uint32_t va, uint32_t size) {
    if (t < 0 || t >= MAX_GSP_QUEUES || slot < 0 || slot >= 15 || !snaps[t][slot].data) return NULL;
    uint8_t *d = snaps[t][slot].data;
    snaps[t][slot].data = NULL; snap_n--;
    if (snaps[t][slot].va != va || snaps[t][slot].size != size) { free(d); return NULL; }
    return d;
}

static pthread_cond_t gx_wake = PTHREAD_COND_INITIALIZER, gx_idle = PTHREAD_COND_INITIALIZER;
static uint64_t spin_until, spin_ns = 300000;   /* the worker's look-again window after a command (PWR_GX_SPIN_US, 0 = off) */
static int gx_pending;
static pthread_t gx_thread;

/* under G: copy the oldest command of any thread's queue into w and remove it (its queue and slot in qs[2]); 0 when all
   queues are empty */
static int pop_gx(uint32_t w[8], int qs[2]) {
    if (!shmem || !shmem->addr) return 0;
    for (int t = 0; t < nthreads; t++) {
        uint32_t q = sh() + 0x800u + 0x200u * (uint32_t)t, old = __atomic_load_n(hdr(q), __ATOMIC_SEQ_CST), nw;
        do {   /* the game appends while we pop: one compare-and-swap of the header (see push_irq_now) */
            uint32_t first = old & 0xff, count = (old >> 8) & 0xff;
            if (!count) break;
            uint32_t c = q + 0x20u + 0x20u * (first % 15);
            for (int k = 0; k < 8; k++) w[k] = rd32(c + 4u * (uint32_t)k);
            qs[0] = t; qs[1] = (int)(first % 15);
            nw = (old & 0xffff0000u) | (count - 1) << 8 | (first + 1) % 15;
        } while (!__atomic_compare_exchange_n(hdr(q), &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        if ((old >> 8) & 0xff) return 1;
    }
    return 0;
}

/* one GX command, without G (the caller set gx_busy): the worker's, or the game thread's own (gx_inline) */
unsigned g_gsp_gx_cmds;
static void gx_exec(const uint32_t *w, uint8_t *snap, int irqs[2]) {
    g_gsp_gx_cmds++;
    GXT("start %02x %08x %x\n", w[0] & 0xff, w[1], w[2]);
    k_ev(0x10, w[0] & 0xff, w[2]);
    int irq = -1, irq2 = -1;
    switch (w[0] & 0xff) {
    case 0x00: pica_vk_sync_range(w[1], w[1] + w[3]); pica_vk_sync_range(w[2], w[2] + w[3]); memcpy(g_mem + w[2], g_mem + w[1], w[3]); pica_vk_scanout_invalidate(w[2], w[2] + w[3]); irq = GSP_IRQ_DMA; break;   /* RequestDma */
    case 0x01: {                                                                                  /* ProcessCommandList */
        static int ll = -1; if (ll < 0) ll = getenv("PWR_PICA_LISTLOG") != NULL;
        uint64_t h0 = 0, h1 = 0;   /* PWR_PICA_LISTLOG: did the game write into the list while it ran? */
        for (uint32_t i = 0; ll && i + 4 <= w[2]; i += 4) h0 = h0 * 1000003u + rd32(w[1] + i);
        k_ev(0x12, w[1], w[2] | (snap ? 0x80000000u : 0));
        { void pica_run_buf_timed(const uint8_t *buf, uint32_t va, uint32_t size); pica_run_buf_timed(snap ? snap : g_mem + w[1], w[1], w[2]); }
        irq = GSP_IRQ_P3D;
        for (uint32_t i = 0; ll && i + 4 <= w[2]; i += 4) h1 = h1 * 1000003u + rd32(w[1] + i);
        if (ll) { uint32_t d = 0; for (int t = 0; t < nthreads; t++) d += rd8(sh() + 0x801u + 0x200u * (uint32_t)t);
                  fprintf(stderr, "gsp: ran list va %08x size %x%s, %u commands still queued%s\n", w[1], w[2], snap ? " (copy)" : "", d, h0 != h1 ? ", guest copy changed while running" : ""); }
        { static int once; if (ll && !once) { once = 1; fprintf(stderr, "gsp: shared memory at %08x, %d queues\n", sh(), nthreads); } }
        free(snap);
        break;
    }
    case 0x02:                                                                                    /* MemoryFill */
        if (w[1]) pica_vk_discard_range(w[1], w[3]);   /* render targets' bytes on their way there: overwritten anyway */
        if (w[4]) pica_vk_discard_range(w[4], w[6]);
        memory_fill(w[1], w[2], w[3], w[7] & 0xffff); memory_fill(w[4], w[5], w[6], w[7] >> 16);
        if (w[1]) { pica_vk_filled(w[1], w[3]); pica_vk_scanout_invalidate(w[1], w[3]); }
        if (w[4]) { pica_vk_filled(w[4], w[6]); pica_vk_scanout_invalidate(w[4], w[6]); }
        if (w[1]) irq = GSP_IRQ_PSC0;
        if (w[4]) irq2 = GSP_IRQ_PSC1;
        break;
    case 0x03: {
        uint32_t in_end = w[1] + (w[3] & 0xffff) * (w[3] >> 16) * 4, out_end = w[2] + (w[4] & 0xffff) * (w[4] >> 16) * 4;
        pica_vk_gx_reads(w[1], in_end);
        if (pica_vk_transfer_gpu(w[1], w[2], w[3], w[4], w[5])) { irq = GSP_IRQ_PPF; break; }   /* the bloom chain, on the GPU */
        /* the scan-out copy at the internal resolution, from the target's image: then the guest bytes are not needed (video.c
           shows that image), and the GPU need not have finished the target */
        int gpu_scan = pica_vk_display_transfer(w[1], w[2], w[3], w[4], w[5]);
        /* (the bottom screen's pixels for Samus Returns' compositor are read back from its scan-out copy: video.c) */
        if (!gpu_scan) {
            pica_vk_sync_range(w[1], in_end); pica_vk_sync_range(w[2], out_end);
            display_transfer(w[1], w[2], w[3], w[4], w[5]);
        }
        irq = GSP_IRQ_PPF;
        break;
    }
    case 0x04:
        if (getenv("PWR_GX_COPYLOG")) {   /* each distinct TextureCopy once: addresses, size, line width/gap, flags */
            static uint32_t seen[32][2]; static int ns; int known = 0;
            for (int i = 0; i < ns; i++) known |= seen[i][0] == w[1] && seen[i][1] == w[2];
            if (!known && ns < 32) { seen[ns][0] = w[1]; seen[ns][1] = w[2]; ns++;
                fprintf(stderr, "gx: TextureCopy %08x -> %08x size %x in %08x out %08x flags %08x\n", w[1], w[2], w[3], w[4], w[5], w[6]); }
        }
        {   /* the bytes written, gaps included: GPU-only textures wholly there need no bytes any more (this copy replaces
               them), anything else on its way there lands first */
            uint32_t ow = (w[5] & 0xffff) * 16, og = (w[5] >> 16) * 16, ospan = ow ? w[3] + (w[3] / ow + 1) * og : w[3];
            pica_vk_alias_discard(w[2], w[2] + ospan);
            pica_vk_sync_range(w[2], w[2] + ospan);
        }
        pica_vk_gx_reads(w[1], w[1] + w[3]); pica_vk_scanout_invalidate(w[2], w[2] + w[3]);
        /* a render target's whole tile rows: the copy at the internal resolution stands for it (its guest bytes are made
           when needed); otherwise the CPU copies the guest bytes */
        if (!pica_vk_texture_copy(w[1], w[2], w[3], w[4], w[5])) {
            uint32_t iw = (w[4] & 0xffff) * 16, ig = (w[4] >> 16) * 16, ispan = iw ? w[3] + (w[3] / iw + 1) * ig : w[3];
            pica_vk_sync_range(w[1], w[1] + ispan);
            texture_copy(w[1], w[2], w[3], w[4], w[5]);
        }
        irq = GSP_IRQ_PPF; break;
    case 0x05: break;                                                                             /* FlushCacheRegions */
    default: fprintf(stderr, "gsp: unknown GX command %02x\n", w[0] & 0xff);
    }
    {   /* PWR_GXDUMP=<game frame>: VRAM after every GX command of that frame, to gxdump_<n>_<command>.bin (6 MiB each) */
        static int dump_frame = -2; if (dump_frame == -2) dump_frame = getenv("PWR_GXDUMP") ? atoi(getenv("PWR_GXDUMP")) : -1;
        if (dump_frame >= 0 && (int)g_gsp_top_swaps == dump_frame) {
            static int seq; char fn[64]; snprintf(fn, sizeof fn, "gxdump_%03d_%02x.bin", seq++, w[0] & 0xff);
            FILE *f = fopen(fn, "wb"); if (f) { fwrite(g_mem + 0x1F000000, 1, 0x600000, f); fclose(f); }
            fprintf(stderr, "gxdump: %s %02x %08x %08x %08x %08x\n", fn, w[0] & 0xff, w[1], w[2], w[3], w[4]);
        }
    }
    k_trace("gsp: GX %02x %08x %08x %08x %08x", w[0] & 0xff, w[1], w[2], w[3], w[4]);
    { static int gt = -1; if (gt < 0) gt = getenv("PWR_GX_TRACE") != NULL;
      if (gt && (w[0] & 0xff) != 1) fprintf(stderr, "gx: %02x %08x %08x %08x %08x %08x %08x %08x\n", w[0] & 0xff, w[1], w[2], w[3], w[4], w[5], w[6], w[7]); }
    GXT("end %02x\n", w[0] & 0xff);
    k_ev(0x11, w[0] & 0xff, 0);
    irqs[0] = irq; irqs[1] = irq2;
}

/* under G, after a command: its time, its interrupts (held back while TriggerCmdReqQueue waits), a VBlank held back */
static void gx_done(const uint32_t *w, uint64_t t0, const int irqs[2]) {
    { unsigned c = w[0] & 7; gxs_ns[c] += gxs_now() - t0; gxs_n[c]++; }
    if (irqs[0] >= 0) push_irq(irqs[0]);
    if (irqs[1] >= 0) push_irq(irqs[1]);
    /* a VBlank held back while the worker lagged arrives as soon as it has caught up, not a whole tick later: above
       60 FPS a tick is under 7 ms, and the game's few commands per frame often span one */
    if (vblank_held) {
        int lag = 0;
        for (int q = 0; shmem && shmem->addr && q < nthreads; q++) lag += rd8(sh() + 0x801u + 0x200u * (uint32_t)q);
        if (lag <= 1) { GXT("vblank (held, delivered)\n"); deliver_vblank(); gxs_late++; }
    }
}

static void *gx_worker(void *arg) {
    (void)arg;
    { const char *e = getenv("PWR_GX_SPIN_US"); if (e) spin_ns = (uint64_t)atoi(e) * 1000u; }
    pthread_mutex_lock(&G);
    for (;;) {
        uint32_t w[8]; int qs[2];
        for (;;) {   /* gx_busy: TriggerCmdReqQueue runs commands itself (gx_inline) */
            gx_pending = 0;
            if (!gx_busy && pop_gx(w, qs)) break;
            /* the game queues its next command within ~0.1 ms of the last one's interrupt, and a thread woken from a wait can take
               0.15-0.35 ms to run when the PC is lightly loaded (60 FPS): look again for a while before sleeping */
            if (spin_until) {
                uint64_t n = gxs_now();
                if (n < spin_until) { pthread_mutex_unlock(&G); sched_yield(); pthread_mutex_lock(&G); continue; }
                spin_until = 0;
            }
            pthread_cond_wait(&gx_wake, &G);
        }
        gx_busy = 1;
        uint64_t gxs_t0 = gxs_now();
        if (gxl_trig) { gxl_ts += gxs_t0 - gxl_trig; gxl_tsn++; gxl_trig = 0; }
        uint8_t *snap = (w[0] & 0xff) == 0x01 ? take_snapshot(qs[0], qs[1], w[1], w[2]) : NULL;
        pthread_mutex_unlock(&G);
        int irqs[2]; gx_exec(w, snap, irqs);
        pthread_mutex_lock(&G);
        gx_busy = 0; pthread_cond_broadcast(&gx_idle);
        gx_done(w, gxs_t0, irqs);
        gxl_end = gxs_now();
        spin_until = gxl_end + spin_ns;
    }
    return NULL;
}

/* under G, gx_busy clear: TriggerCmdReqQueue with only small commands queued, which the game waits for (PWR_GX_SYNC_MAX),
   runs them itself, in order, instead of waking the worker and waiting to be woken in turn: most of the game's ~30 GX
   commands a frame are small and run one at a time (each waits for the one before), and with the PC lightly loaded
   (60 FPS) the two wake-ups took 0.15-0.35 ms each, more than the commands. PWR_GX_NO_INLINE=1: the worker. */
static void gx_inline(void) {
    gx_busy = 1;
    uint32_t w[8];
    int qs[2];
    while (pop_gx(w, qs)) {
        uint64_t t0 = gxs_now();
        uint8_t *snap = (w[0] & 0xff) == 0x01 ? take_snapshot(qs[0], qs[1], w[1], w[2]) : NULL;
        pthread_mutex_unlock(&G);
        int irqs[2]; gx_exec(w, snap, irqs);
        pthread_mutex_lock(&G);
        gx_done(w, t0, irqs);
    }
    gx_busy = 0; pthread_cond_broadcast(&gx_idle);
    gxl_end = gxs_now();
}

void gsp_handle(KThread *t, uint32_t *cmd, KObject *s) {
    (void)t; (void)s;
    uint32_t id = cmd[0] >> 16;
    pthread_mutex_lock(&G);
    switch (id) {
    case 0x01: case 0x02: {   /* WriteHWRegs(offset, size, data) / WriteHWRegsWithMask(offset, size, data, mask) */
        uint32_t off = cmd[1], size = cmd[2], data = cmd[4], mask = id == 2 ? cmd[6] : 0;
        if (!hwregs) hwregs = calloc(HWREG_SIZE / 4, 4);
        for (uint32_t i = 0; i + 4 <= size && off + i < HWREG_SIZE; i += 4) {
            uint32_t v = rd32(data + i), m = mask ? rd32(mask + i) : 0xffffffffu, *r = &hwregs[(off + i) / 4];
            *r = (*r & ~m) | (v & m);
        }
        if ((off >= 0x400400 && off < 0x400600)) screen_from_regs = 1;
        k_trace("gsp: WriteHWRegs%s %06x size=%x first=%08x", id == 2 ? "WithMask" : "", off, size, size >= 4 ? rd32(data) : 0);
        cmd[0] = HDR(id, 1, 0); cmd[1] = RES_OK; break;
    }
    case 0x04: case 0x08: case 0x09: case 0x0B: case 0x10: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1E: case 0x1F:
        /* WriteHWRegs, WriteHWRegsWithMask, ReadHWRegs (zeros), Flush/InvalidateDataCache, SetLcdForceBlack,
           SetAxiConfigQoSMode, AcquireRight, ReleaseRight, ImportDisplayCaptureInfo, Save/RestoreVramSysArea, ... */
        if (id == 0x08) { static int ll = -1; if (ll < 0) ll = getenv("PWR_PICA_LISTLOG") != NULL;
                          if (ll) fprintf(stderr, "gsp: FlushDataCache %08x size %x\n", cmd[1], cmd[2]); }
        if (id == 0x04) {   /* ReadHWRegs(offset, size) -> static buffer */
            uint32_t off = cmd[1], size = cmd[2], buf = cmd[0x41];
            for (uint32_t i = 0; buf && i + 4 <= size; i += 4) wr32(buf + i, hw(off + i));
        }
        cmd[0] = HDR(id, 1, 0); cmd[1] = RES_OK; break;
    case 0x05: { /* SetBufferSwap(screen, FrameBufferInfo) */
        int scr = cmd[1] & 1;
        screens[scr].addr = cmd[3]; screens[scr].stride = cmd[5]; screens[scr].format = cmd[6] & 7; screens[scr].valid = 1;
        cmd[0] = HDR(5, 1, 0); cmd[1] = RES_OK; break;
    }
    case 0x0C: /* TriggerCmdReqQueue */
        if (!gx_thread) pthread_create(&gx_thread, NULL, gx_worker, NULL);
        {   /* PWR_PICA_LISTLOG=1: command lists as they are queued (pica.c logs them as they run) */
            static int ll = -1; if (ll < 0) ll = getenv("PWR_PICA_LISTLOG") != NULL;
            for (int q = 0; ll && shmem && shmem->addr && q < nthreads; q++) {
                uint32_t b = sh() + 0x800u + 0x200u * (uint32_t)q, n = rd8(b + 1);
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t c = b + 0x20u + 0x20u * ((rd8(b) + i) % 15);
                    if ((rd32(c) & 0xff) == 1) fprintf(stderr, "gsp: queued list va %08x size %x (queue %d, slot %u/%u, thread %d)\n",
                                                        rd32(c + 4), rd32(c + 8), q, i, n, k_current_index());
                }
            }
        }
        GXT("trigger by k%d\n", k_current_index());
        { uint64_t n = gxs_now(); if (gxl_end) { gxl_et += n - gxl_end; gxl_etn++; gxl_end = 0; } if (!gx_busy) gxl_trig = n; }
        {   /* PWR_GX_SYNC_MAX=<bytes>: the 3DS GPU finishes a small command list before the CPU gets far, and the game then reuses
               the vertex and texture memory (the face library builds a Mii face from many tiny lists in one work buffer). Our GPU
               thread runs behind, so wait here until it has finished when every queued list is that small. Big scene lists keep
               overlapping with the game's own work. Completion interrupts are held until this call returns. */
            static uint32_t sync_max = 0xffffffffu; if (sync_max == 0xffffffffu) { const char *e = getenv("PWR_GX_SYNC_MAX"); sync_max = e ? (uint32_t)strtoul(e, NULL, 0) : 0x4000u; }
            uint32_t big = 0, nq = 0;
            for (int q = 0; sync_max && shmem && shmem->addr && q < nthreads; q++) {
                uint32_t b = sh() + 0x800u + 0x200u * (uint32_t)q, n = rd8(b + 1);
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t c = b + 0x20u + 0x20u * ((rd8(b) + i) % 15); nq++;
                    if ((rd32(c) & 0xff) == 1 && rd32(c + 8) > big) big = rd32(c + 8);
                }
            }
            static int inl = -1; if (inl < 0) inl = getenv("PWR_GX_NO_INLINE") == NULL;
            int here = inl && sync_max && nq && big <= sync_max && !gx_busy;
            if (!here) { gx_pending = 1; pthread_cond_signal(&gx_wake); }
            else gxl_trig = 0;
            if (sync_max && nq && big <= sync_max) {
                irq_hold = 1;
                if (here) gx_inline();
                for (int tries = 0; !here && tries < 400; tries++) {   /* at most ~2 s */
                    int lag = gx_busy || gx_pending;
                    for (int q = 0; q < nthreads; q++) lag += rd8(sh() + 0x801u + 0x200u * (uint32_t)q);
                    if (!lag) break;
                    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 5000000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                    pthread_cond_timedwait(&gx_idle, &G, &ts);
                }
                irq_hold = 0;
                for (int i = 0; i < nheld; i++) push_irq_now(held[i]);
                nheld = 0;
            }
        }
        cmd[0] = HDR(0xC, 1, 0); cmd[1] = RES_OK; break;
    case 0x13: { /* RegisterInterruptRelayQueue(flags, event) -> 0x2A07 (first init), thread index, shared memory */
        int first = !shmem;
        if (!shmem) { shmem = k_alloc(KO_SHMEM); shmem->size = 0x1000; }
        int ti = nthreads < MAX_GSP_THREADS ? nthreads++ : MAX_GSP_THREADS - 1;
        events[ti] = k_obj(cmd[3]);
        k_trace("gsp: RegisterInterruptRelayQueue flags=%x event=%x (%s) -> thread %d", cmd[1], cmd[3], events[ti] ? "ok" : "BAD HANDLE", ti);
        cmd[0] = HDR(0x13, 2, 2); cmd[1] = first ? 0x2A07u : RES_OK; cmd[2] = (uint32_t)ti; cmd[3] = 0x04000000; cmd[4] = k_new_handle(shmem);
        shmem->refs++;
        break;
    }
    case 0x14: cmd[0] = HDR(0x14, 1, 0); cmd[1] = RES_OK; break;   /* UnregisterInterruptRelayQueue */
    default:
        fprintf(stderr, "svc: unhandled gsp::Gpu command %08x (replying success)\n", cmd[0]);
        cmd[0] = HDR(id, 1, 0); cmd[1] = RES_OK;
    }
    pthread_mutex_unlock(&G);
}

void gsp_hang_report(void) {
    fprintf(stderr, "hang: gsp gx_busy %d gx_pending %d irq_hold %d nheld %d snapshots %d\n", gx_busy, gx_pending, irq_hold, nheld, snap_n);
    for (int t = 0; shmem && shmem->addr && t < nthreads; t++)
        fprintf(stderr, "hang:  gsp thread %d: interrupt queue header %08x, GX queue header %08x, event %s\n", t,
                rd32(sh() + 0x40u * (uint32_t)t), rd32(sh() + 0x800u + 0x200u * (uint32_t)t), events[t] ? (events[t]->signaled ? "signaled" : "clear") : "none");
    extern void dsp_hang_report(void); dsp_hang_report();
}

/* ---- save states (state.h) ---- */
/* the worker idle and nothing queued (the game's threads are frozen: nothing new comes); G stays held until gsp_release,
   also when it returns -1 (timed out) */
int gsp_quiesce(int timeout_ms) {
    pthread_mutex_lock(&G);
    for (int i = 0; i < timeout_ms; i++) {
        int lag = gx_busy || gx_pending;
        for (int q = 0; shmem && shmem->addr && q < nthreads; q++) lag += rd8(sh() + 0x801u + 0x200u * (uint32_t)q);
        if (!lag && !irq_hold) { pica_vk_sync_all(); return 0; }   /* render targets' bytes all in guest memory */
        if (!gx_busy && lag) { gx_pending = 1; pthread_cond_signal(&gx_wake); }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 1000000; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&gx_idle, &G, &ts);
    }
    return -1;   /* G held all the same */
}
void gsp_release(void) { pthread_mutex_unlock(&G); }

void gsp_state_save(SBuf *b) {   /* under G (gsp_quiesce) */
    sb_u32(b, ks_id(shmem)); sb_u32(b, (uint32_t)nthreads);
    for (int i = 0; i < MAX_GSP_THREADS; i++) sb_u32(b, ks_id(events[i]));
    SB(b, screens); sb_u32(b, (uint32_t)screen_from_regs); sb_u32(b, g_gsp_top_swaps); sb_u32(b, (uint32_t)vblank_held);
    uint32_t nz = 0;   /* the GPU registers written through WriteHWRegs: the few that are set */
    for (uint32_t i = 0; hwregs && i < HWREG_SIZE / 4; i++) nz += hwregs[i] != 0;
    sb_u32(b, nz);
    for (uint32_t i = 0; hwregs && i < HWREG_SIZE / 4; i++) if (hwregs[i]) { sb_u32(b, i); sb_u32(b, hwregs[i]); }
}

void gsp_state_load(SRd *r) {    /* under G (gsp_quiesce) */
    shmem = ks_obj(sr_u32(r)); nthreads = (int)sr_u32(r); if (nthreads > MAX_GSP_THREADS) nthreads = MAX_GSP_THREADS;
    for (int i = 0; i < MAX_GSP_THREADS; i++) events[i] = ks_obj(sr_u32(r));
    SR(r, screens); screen_from_regs = (int)sr_u32(r); (void)sr_u32(r); vblank_held = (int)sr_u32(r);   /* the frame count goes on (a host counter) */
    if (!hwregs) hwregs = calloc(HWREG_SIZE / 4, 4);
    memset(hwregs, 0, HWREG_SIZE);
    uint32_t nz = sr_u32(r);
    for (uint32_t k = 0; k < nz && !r->bad; k++) { uint32_t i = sr_u32(r), v = sr_u32(r); if (i < HWREG_SIZE / 4) hwregs[i] = v; }
    for (int t = 0; t < MAX_GSP_QUEUES; t++) for (int s = 0; s < 15; s++) { free(snaps[t][s].data); snaps[t][s].data = NULL; }
    snap_n = 0; irq_hold = 0; nheld = 0; gx_pending = 0; spin_until = 0;
}
