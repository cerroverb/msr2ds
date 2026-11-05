/* PICA200 command-list processor.
 * A list is a sequence of {u32 param, u32 header} pairs. header: bits 0-15 register id, 16-19 byte-enable mask,
 * 20-27 number of extra params that follow, bit 31 = extra params go to consecutive registers (else same register).
 * The list is padded so every command starts 8-byte aligned. Draws are triggered by writes to 0x22E (DrawArrays)
 * and 0x22F (DrawElements). Upload ports and draws are handed to the software pipeline in pica_sw.c. */
#include "pica_int.h"
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#define PICA_REG_COUNT 0x300
uint32_t pica_regs[PICA_REG_COUNT];

#define regs pica_regs
void pica_sw_write(uint32_t id, uint32_t v);
void pica_sw_draw(int indexed);
void pica_sw_imm_flush(void);
static uint32_t n_lists, n_draws;
static pthread_mutex_t P = PTHREAD_MUTEX_INITIALIZER;

static FILE *dump_f; static long dump_list = -1;

static void write_reg(uint32_t id, uint32_t value, uint32_t mask) {
    if (dump_f) fprintf(dump_f, "%03x %08x %x\n", id, value, mask);
    {   /* PWR_PICA_REGLOG=<hex reg>[,<hex reg>...]: log every write to those registers with the list number */
        static long rl[8]; static int nrl = -1;
        if (nrl < 0) { const char *e = getenv("PWR_PICA_REGLOG"); nrl = 0; while (e && *e && nrl < 8) { char *x; rl[nrl++] = strtol(e, &x, 16); e = *x ? x + 1 : x; } }
        for (int i = 0; i < nrl; i++)
            if (rl[i] == (long)id) fprintf(stderr, "pica: list %u reg %03x = %08x mask %x (fb %08x)\n", n_lists, id, value, mask, regs[0x11D]);
    }
    if (id >= PICA_REG_COUNT) return;
    if (id >= 0x0A8 && id <= 0x0B7 && value) {   /* procedural texture registers: say so the first time */
        static int seen; if (!seen) { seen = 1; fprintf(stderr, "pica: first procedural texture register write %03x = %08x, list %u\n", id, value, n_lists); }
    }
    /* immediate-mode vertices are drawn with the state they were sent under */
    if (id < 0x232 || id > 0x235) pica_sw_imm_flush();
    uint32_t m = (mask & 1 ? 0xffu : 0) | (mask & 2 ? 0xff00u : 0) | (mask & 4 ? 0xff0000u : 0) | (mask & 8 ? 0xff000000u : 0);
    regs[id] = (regs[id] & ~m) | (value & m);
    if (id >= 0x232 || (id >= 0x1C5 && id <= 0x1CF) || id == 0x0E6 || (id >= 0x0E8 && id <= 0x0EF)) pica_sw_write(id, regs[id]);
    if ((id == 0x22E || id == 0x22F) && (value & 1)) {
        {   /* first real use of fog / gas / stencil (none seen in the title, menus, trial flight or the first mission): say where */
            static int seen[3]; int fm = regs[0x0E0] & 7, k = fm == 5 ? 0 : fm == 7 ? 1 : (regs[0x105] & 1) && (regs[0x116] & 3) == 3 ? 2 : -1;
            if (k >= 0 && !seen[k]) { seen[k] = 1; static const char *nm[3] = { "fog", "gas (not implemented)", "stencil" };
                fprintf(stderr, "pica: first draw with %s, list %u, colour buffer %08x, 0x0E0=%08x 0x105=%08x 0x106=%08x\n", nm[k], n_lists, regs[0x11D] << 3, regs[0x0E0], regs[0x105], regs[0x106]); }
            static int seen_pt;   /* texture unit 3 = procedural texture (not implemented; the game never looks up its uniforms) */
            if ((regs[0x080] & 0x400) && !seen_pt) { seen_pt = 1;
                fprintf(stderr, "pica: first draw with a procedural texture (not implemented), list %u, colour buffer %08x, 0x080=%08x 0x0A8=%08x\n", n_lists, regs[0x11D] << 3, regs[0x080], regs[0x0A8]); }
        }
        /* renderer checks without a scene that uses these: PWR_TEST_FOG=1 forces fog on 3D draws (red, full at the far plane), PWR_TEST_STENCIL=1
           lets only the first fragment drawn at each pixel through (stencil == 0, increment on pass) on D24S8 targets */
        static int tfog = -1, tst; if (tfog < 0) { tfog = getenv("PWR_TEST_FOG") != NULL; tst = getenv("PWR_TEST_STENCIL") != NULL;
            /* z-buffer depth sits at 0.99+ for most of a scene: no fog up to entry 127, which ramps to full fog over depth 0.992-1 */
            if (tfog) for (int i = 0; i < 128; i++) { extern uint32_t fog_lut[128]; fog_lut[i] = 2047u << 13 | (i == 127 ? (uint32_t)-2047 & 0x1fff : 0); } }
        uint32_t s0e0 = regs[0x0E0], s0e1 = regs[0x0E1], s105 = regs[0x105], s106 = regs[0x106];
        if (tfog && (regs[0x107] & 1)) { regs[0x0E0] = (regs[0x0E0] & ~7u) | 5; regs[0x0E1] = 0x0000ff; }   /* depth-tested (3D) draws only */
        if (tst) { regs[0x105] = 1 | 2 << 4 | 0xffu << 8 | 0u << 16 | 0xffu << 24; regs[0x106] = 3u << 8; }
        n_draws++; pica_sw_draw(id == 0x22F);
        if (tfog || tst) { regs[0x0E0] = s0e0; regs[0x0E1] = s0e1; regs[0x105] = s105; regs[0x106] = s106; }
    }
    if (id == 0x23C || id == 0x23D) { static int once; if (!once) { once = 1; fprintf(stderr, "pica: command-buffer jump used (not implemented)\n"); } }
}

/* buf holds the list's bytes: guest memory, or the copy gsp.c took when the game queued the list */
void pica_run_buf(const uint8_t *buf, uint32_t va, uint32_t size) {
    #define LW(a) ({ uint32_t w_; memcpy(&w_, buf + ((a) - va), 4); w_; })
    pthread_mutex_lock(&P);
    n_lists++;
    { static int ll = -1; if (ll < 0) ll = getenv("PWR_PICA_LISTLOG") != NULL;   /* PWR_PICA_LISTLOG=1: address/size of every list */
      if (ll) fprintf(stderr, "pica: run list %u va %08x size %x\n", n_lists, va, size); }
    if (dump_list < 0) { const char *e = getenv("PWR_PICA_DUMP"); dump_list = e ? atol(e) : 0; }   /* dump list #N's writes */
    if (dump_list > 0 && (long)n_lists == dump_list) dump_f = fopen("pica_dump.txt", "w");
    {   /* PWR_PICA_DUMP_AT=<seconds>: dump the first list after that much run time */
        static double at = -1, t0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
        if (at < 0) { const char *e = getenv("PWR_PICA_DUMP_AT"); at = e ? atof(e) : 0; t0 = now; }
        if (at > 0 && now - t0 >= at) { at = 0; dump_f = fopen("pica_dump.txt", "w"); fprintf(stderr, "pica: dumping list %u\n", n_lists); }
    }
    {   /* PWR_PICA_DUMP_FB=<lo va>-<hi va>: dump every list that sets a colour buffer in that range (at most 16) */
        static long lo = -2, hi; static int nd;
        if (lo == -2) { const char *e = getenv("PWR_PICA_DUMP_FB"); lo = -1; if (e) { lo = strtol(e, NULL, 16); const char *d = strchr(e, '-'); hi = d ? strtol(d + 1, NULL, 16) : lo + 1; } }
        for (uint32_t q = va; lo >= 0 && !dump_f && nd < 16 && q + 8 <= va + size; ) {
            uint32_t h = LW(q + 4), x = (h >> 20) & 0xff, cs = h >> 31, id = h & 0xffff;
            for (uint32_t i = 0; i <= x; i++) {
                uint32_t r = cs ? id + i : id, v = i ? LW(q + 4 + 4 * i) : LW(q);
                long cb = r == 0x11D ? (long)pa_to_va((v & 0x0fffffff) << 3) : -1;
                if (cb >= lo && cb < hi) { char fn[64]; snprintf(fn, sizeof fn, "pica_dump_fb_%u.txt", n_lists); dump_f = fopen(fn, "w"); nd++;
                    fprintf(dump_f, "# at start: 2ba %08x 11e %08x 11d %08x 1c5..: lists before %u\n", regs[0x2BA], regs[0x11E], regs[0x11D], n_lists);
                    fprintf(stderr, "pica: dumping list %u (colour buffer %08lx)\n", n_lists, (unsigned long)cb); break; }
            }
            q += 8 + 4 * x + ((x & 1) ? 4 : 0);
        }
    }
    uint32_t p = va, end = va + size;
    while (p + 8 <= end) {
        uint32_t param = LW(p), hdr = LW(p + 4);
        uint32_t id = hdr & 0xffff, mask = (hdr >> 16) & 15, extra = (hdr >> 20) & 0xff, consec = hdr >> 31;
        write_reg(id, param, mask);
        p += 8;
        for (uint32_t i = 0; i < extra; i++, p += 4) write_reg(consec ? id + 1 + i : id, LW(p), mask);
        if (extra & 1) p += 4;   /* keep 8-byte alignment */
    }
    pica_sw_imm_flush();
    if (pica_vk_enabled()) pica_vk_flush();
    if (dump_f) { fclose(dump_f); dump_f = NULL; }
    pthread_mutex_unlock(&P);
    #undef LW
}

void pica_run(uint32_t va, uint32_t size) { pica_run_buf(g_mem + va, va, size); }
void pica_run_buf_timed(const uint8_t *buf, uint32_t va, uint32_t size) {
    extern unsigned long long g_prof_list_ns; extern uint64_t prof_ns(void);
    uint64_t t0 = prof_ns(); pica_run_buf(buf, va, size); g_prof_list_ns += prof_ns() - t0;
}

void pica_stats(uint32_t *lists, uint32_t *draws) { *lists = n_lists; *draws = n_draws; }
