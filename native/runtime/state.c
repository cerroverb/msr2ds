/* Save states (the settings overlay's Quick save and Quick load). A state is the guest's memory and the runtime's state in
 * sections (state.h). Saving stops the game's threads where their registers are their whole state (k_freeze), lets the
 * GPU worker finish what was queued, copies everything (a few tens of milliseconds) and lets the game go on; compressing
 * and writing the file happen on another thread. Loading reads the file first, then stops the game, makes every guest
 * thread leave what it was doing (k_abandon_all), puts the memory and the state back and starts the threads from their
 * saved states; a thread that was in a wait issues it again.
 *
 * The file: "MSRNATST", the format version, flags (1 = zlib), the payload's size, the loaded program's hash (the dump
 * and the game mods' code: a state from other game files is refused), then the payload: sections, each a 4-character
 * tag, a 64-bit size and its data. The game's code (.text) is not in it: it is the dump's, and its words the frame
 * rate and the extras patch follow the current settings. */
#include "state.h"
#include "gpu.h"
#include "runtime.h"
#include "msr/msr.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif
#ifdef MSR_HAVE_ZLIB
#include <zlib.h>
#endif

#define STATE_VERSION 1
extern uint32_t g_text_lo, g_text_hi;   /* main.c: the game's code, as loaded */
extern uint64_t g_code_hash;

void sb_put(SBuf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 65536;
        while (c < b->n + n) c *= 2;
        uint8_t *p = realloc(b->p, c);
        if (!p) { fprintf(stderr, "state: out of memory\n"); abort(); }
        b->p = p; b->cap = c;
    }
    memcpy(b->p + b->n, d, n); b->n += n;
}
void sr_get(SRd *r, void *d, size_t n) {
    if (r->off + n > r->n) { memset(d, 0, n); r->bad = 1; r->off = r->n; return; }
    memcpy(d, r->p + r->off, n); r->off += n;
}

static void section(SBuf *out, const char *tag, SBuf *body) {
    sb_put(out, tag, 4); uint64_t n = body->n; SB(out, n); if (body->n) sb_put(out, body->p, body->n);
    free(body->p); memset(body, 0, sizeof *body);
}
static int find_section(const uint8_t *p, size_t n, const char *tag, SRd *r) {
    size_t off = 0;
    while (off + 12 <= n) {
        uint64_t len; memcpy(&len, p + off + 4, 8);
        if (len > n - off - 12) return -1;
        if (!memcmp(p + off, tag, 4)) { r->p = p + off + 12; r->n = (size_t)len; r->off = 0; r->bad = 0; return 0; }
        off += 12 + (size_t)len;
    }
    return -1;
}

/* ---- the guest's memory ---- */
typedef struct { uint32_t addr, size; } Range;
static int add_range(Range *out, int n, int max, uint64_t lo, uint64_t hi) {   /* [lo, hi) without the code */
    if (hi <= lo) return n;
    if (lo < g_text_hi && hi > g_text_lo) {
        n = add_range(out, n, max, lo, g_text_lo);
        return add_range(out, n, max, g_text_hi, hi);
    }
    if (n < max) { out[n].addr = (uint32_t)lo; out[n].size = (uint32_t)(hi - lo); n++; }
    return n;
}
/* what the game has of its 4 GiB: the committed pages (Windows commits a page when it is first touched), or the memory map */
static int memory_ranges(Range *out, int max) {
    int n = 0;
#ifdef _WIN32
    uint64_t a = 0;
    while (a < (1ull << 32) && n < max) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(g_mem + a, &mbi, sizeof mbi)) break;
        uint64_t base = (uint64_t)((uint8_t *)mbi.BaseAddress - g_mem), end = base + mbi.RegionSize;
        if (end > (1ull << 32)) end = 1ull << 32;
        if (mbi.State == MEM_COMMIT) n = add_range(out, n, max, base, end);
        if (end <= a) break;
        a = end;
    }
#else
    uint32_t base[256], size[256], st[256];
    int nr = k_region_list(base, size, st, 256);
    for (int i = 0; i < nr; i++) n = add_range(out, n, max, base[i], (uint64_t)base[i] + size[i]);
    n = add_range(out, n, max, VA_VRAM, VA_VRAM + 0x600000u);
    n = add_range(out, n, max, VA_DSPRAM, VA_DSPRAM + 0x80000u);
    n = add_range(out, n, max, VA_CONFIG, VA_TLS + 0x200u * 32u);
    n = add_range(out, n, max, 0x10000000u, 0x10400000u);   /* shared memory blocks mapped above the stack */
#endif
    return n;
}

static void memory_save(SBuf *b) {
    static Range rs[8192];
    int n = memory_ranges(rs, 8192);
    uint32_t fix[64]; int nfix = k_state_mem_fixups(fix, 64);
    sb_u32(b, (uint32_t)n);
    sb_put(b, rs, sizeof *rs * (size_t)n);
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        size_t at = b->n;
        sb_put(b, g_mem + rs[i].addr, rs[i].size);
        for (int k = 0; k < nfix; k++)   /* a decrement-and-wait arbiter is issued again: its decrement taken back */
            if (fix[k] >= rs[i].addr && fix[k] + 4 <= rs[i].addr + rs[i].size) { uint32_t v; memcpy(&v, b->p + at + (fix[k] - rs[i].addr), 4); v++; memcpy(b->p + at + (fix[k] - rs[i].addr), &v, 4); }
        total += rs[i].size;
    }
    fprintf(stderr, "state: %d memory ranges, %.1f MiB\n", n, total / 1048576.0);
}

static int memory_load(SRd *r) {
    uint32_t n = sr_u32(r);
    if (n > 65536) return -1;
    Range *rs = malloc(sizeof *rs * (n ? n : 1));
    sr_get(r, rs, sizeof *rs * n);
    if (r->bad) { free(rs); return -1; }
    /* what the game has now goes (pages it touches again come back as zeros), then the state's memory */
    static Range now[8192];
    int nn = memory_ranges(now, 8192);
#ifdef _WIN32
    for (int i = 0; i < nn; i++) VirtualFree(g_mem + now[i].addr, now[i].size, MEM_DECOMMIT);
#else
    for (int i = 0; i < nn; i++) memset(g_mem + now[i].addr, 0, now[i].size);
#endif
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        if (r->off + rs[i].size > r->n) { r->bad = 1; break; }
        rt_mem_commit(rs[i].addr, rs[i].size);
        memcpy(g_mem + rs[i].addr, r->p + r->off, rs[i].size);
        r->off += rs[i].size;
    }
    free(rs);
    return r->bad ? -1 : 0;
}

/* ---- messages for the screen (video.c shows them as the layout label) ---- */
static pthread_mutex_t msg_lock = PTHREAD_MUTEX_INITIALIZER;
static char msg_text[160]; static int msg_new;
static void say(const char *m) {
    pthread_mutex_lock(&msg_lock); snprintf(msg_text, sizeof msg_text, "%s", m); msg_new = 1; pthread_mutex_unlock(&msg_lock);
    fprintf(stderr, "state: %s\n", m);
}
const char *msr_state_message(void) {   /* the newest message once, or NULL */
    static char out[160];
    pthread_mutex_lock(&msg_lock);
    const char *r = NULL;
    if (msg_new) { msg_new = 0; memcpy(out, msg_text, sizeof out); r = out; }
    pthread_mutex_unlock(&msg_lock);
    return r;
}

/* ---- the file ---- */
typedef struct { char magic[8]; uint32_t version, flags; uint64_t size, code_hash; } Header;

static void quick_path(char *out, size_t n) {   /* <user>/states/native-quick.state, beside <user>/msr/settings.ini */
    const char *s = msr_settings_file();
    const char *slash = strrchr(s, '/'), *bslash = strrchr(s, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    char dir[1024]; snprintf(dir, sizeof dir, "%.*s../states", slash ? (int)(slash - s + 1) : 0, s);
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    snprintf(out, n, "%s/native-quick.state", dir);
}

typedef struct { SBuf body; char path[1100]; } SaveJob;
static pthread_t save_thread; static int save_running;
static void *save_job(void *p) {
    SaveJob *j = p;
    Header h = { { 'M', 'S', 'R', 'N', 'A', 'T', 'S', 'T' }, STATE_VERSION, 0, j->body.n, g_code_hash };
    uint8_t *data = j->body.p; size_t size = j->body.n; uint8_t *packed = NULL;
#ifdef MSR_HAVE_ZLIB
    uLongf bound = compressBound((uLong)j->body.n);
    packed = malloc(bound);
    if (packed && compress2(packed, &bound, j->body.p, (uLong)j->body.n, 1) == Z_OK) { data = packed; size = bound; h.flags = 1; }
#endif
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", j->path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(data, 1, size, f) == size;
    if (f) ok &= fclose(f) == 0;
    if (ok) ok = !rt_replace_file(tmp, j->path);
    if (!ok) remove(tmp);
    say(ok ? "Saved" : "The state could not be written");
    fprintf(stderr, "state: %s, %.1f MiB (%.1f MiB in memory)\n", j->path, size / 1048576.0, j->body.n / 1048576.0);
    free(packed); free(j->body.p); free(j);
    return NULL;
}
static void save_wait(void) { if (save_running) { pthread_join(save_thread, NULL); save_running = 0; } }

/* the settings overlay's Quick save, on the main thread between frames */
void msr_state_quick_save(void) {
    save_wait();
    if (k_freeze(1000)) { say("Not saved: the game is busy, try again"); return; }
    if (gsp_quiesce(1000)) { gsp_release(); k_thaw(); say("Not saved: the GPU is busy, try again"); return; }
    dsp_pause(1);
    SaveJob *j = calloc(1, sizeof *j);
    SBuf s = { 0 };
    memory_save(&s); section(&j->body, "MEM ", &s);
    k_state_save(&s); section(&j->body, "KERN", &s);   /* first to name objects (ks_id) */
    services_state_save(&s); section(&j->body, "SERV", &s);
    gsp_state_save(&s); section(&j->body, "GSP ", &s);
    pica_state_save(&s); section(&j->body, "PICA", &s);
    dsp_state_save(&s); section(&j->body, "DSP ", &s);
    fs_state_save(&s); section(&j->body, "FS  ", &s);
    k_state_save_objects(&s); section(&j->body, "OBJS", &s);   /* last: every object named above */
    dsp_pause(0);
    gsp_release();
    k_thaw();
    quick_path(j->path, sizeof j->path);
    say("Saving...");
    if (pthread_create(&save_thread, NULL, save_job, j) == 0) save_running = 1;
    else save_job(j);
}

/* the settings overlay's Quick load */
void msr_state_quick_load(void) {
    save_wait();   /* a save still being written goes first */
    char path[1100]; quick_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) { say("No quick save yet"); return; }
    Header h; uint8_t *data = NULL; size_t n = 0;
    int ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, "MSRNATST", 8) && h.version == STATE_VERSION && h.size < (4ull << 30);
    if (ok) {
        fseek(f, 0, SEEK_END); long end = ftell(f); fseek(f, (long)sizeof h, SEEK_SET);
        size_t packed = end > (long)sizeof h ? (size_t)end - sizeof h : 0;
        uint8_t *raw = malloc(packed ? packed : 1);
        ok = raw && fread(raw, 1, packed, f) == packed;
        if (ok && h.flags & 1) {
#ifdef MSR_HAVE_ZLIB
            uLongf out = (uLongf)h.size; data = malloc(h.size ? h.size : 1);
            ok = data && uncompress(data, &out, raw, (uLong)packed) == Z_OK && out == h.size; n = out;
#else
            ok = 0;
#endif
            free(raw);
        } else { data = raw; n = packed; }
    }
    fclose(f);
    if (!ok) { free(data); say("The quick save could not be read"); return; }
    if (h.code_hash != g_code_hash) { free(data); say("Not loaded: the quick save is from other game files or mods"); return; }
    SRd mem, kern, serv, gsp, pica, dsp, fs, objs;
    if (find_section(data, n, "MEM ", &mem) || find_section(data, n, "KERN", &kern) || find_section(data, n, "SERV", &serv) ||
        find_section(data, n, "GSP ", &gsp) || find_section(data, n, "PICA", &pica) || find_section(data, n, "DSP ", &dsp) ||
        find_section(data, n, "FS  ", &fs) || find_section(data, n, "OBJS", &objs)) { free(data); say("The quick save is damaged"); return; }

    if (k_freeze(1000)) { free(data); say("Not loaded: the game is busy, try again"); return; }
    /* the threads leave first (one waiting in the GX enqueue hook needs the worker to go on), then the worker finishes */
    if (k_abandon_all(3000)) fprintf(stderr, "state: not every thread stopped in time; loading anyway\n");
    if (gsp_quiesce(2000)) fprintf(stderr, "state: the GX worker did not finish in time; loading anyway\n");
    dsp_pause(1);
    int bad = memory_load(&mem);
    k_state_load_objects(&objs);
    bad |= k_state_load(&kern);
    services_state_load(&serv);
    gsp_state_load(&gsp);
    pica_state_load(&pica);
    pica_vk_state_reset();
    dsp_state_load(&dsp);
    fs_state_load(&fs);
    msr_cheats_reset();
    dsp_pause(0);
    gsp_release();
    k_state_resume();
    free(data);
    say(bad ? "Loaded, but the quick save was damaged" : "Loaded");
}
