/* OpenGL PICA200 renderer: pica_vk.c's work on OpenGL 3.3 (with the OpenGL presenter, video_gl.c: --renderer=opengl, or
 * auto where Vulkan has no 1.3 device), for graphics without Vulkan. pica_vk.c's design, in OpenGL's terms:
 *   - Vertex shaders run on the GPU as in pica_vk.c: pica_vsgen.c's translation of the game's program (as GLSL 3.30,
 *     pica_vsgen_glsl) once it has compiled, the interpreter (shaders/pica_vs.vert) until then. The game's vertex bytes
 *     (each loader's range) or the CPU's vec4 attributes are a texture buffer here (storage buffers are OpenGL 4.3), as
 *     is the program. PWR_GL_CPU_VS=1 shades on the CPU (pica_sw.c's worker pool, pica_vk.c's PWR_VK_CPU_VS=1) with
 *     shaders/pica.vert.
 *   - shaders/pica.frag is the Vulkan renderer's, compiled as GLSL 3.30 (its Vulkan-only lines are under #ifdef VULKAN):
 *     specialised for each draw's configuration (pica_vk.c's specialisation constants, as GLSL constants) or generic
 *     (SC_ON 0, reading it from the uniforms: PWR_VK_NOSPEC=1 keeps to that one). Programs are compiled and linked on a
 *     thread of their own (programs, below) and kept with their binaries for the next start (pwr_gl_programs.bin).
 *   - Draws run as they come: OpenGL has no render passes, so there is nothing to gain from Vulkan's deferred replay
 *     grouped by target, and uploads simply come before the draws that follow them. A draw's data goes up in one piece
 *     (dbuf); textures, samplers and framebuffers are bound only when they change.
 *   - Render targets are textures at the internal resolution, uploaded from guest memory when its bytes changed (64-bit
 *     hash) or cleared for a MemoryFill over them. Every colour target drawn in a command list is read back at its end
 *     (filtered to 1x) into a pixel buffer of its own, without waiting; its bytes reach guest memory when something
 *     reads them (a GX command, a texture over them, the VBlank once its fence has signalled, a save state), as
 *     pica_vk.c's downloads do. Landing one is a fence wait and a buffer map: any thread with a context sharing this
 *     one's can do it (the main thread has the presenter's: the VBlank and save states call from there, with the GX
 *     worker idle).
 *   - The GX copies of render targets run on the GPU (pica_vk.c's): the TextureCopy of a target and the bloom chain's
 *     halvings make textures at the internal resolution that draws sample in place of the copied bytes (aliases), whose
 *     own bytes are read back the same way and land only when something reads them; each DisplayTransfer of a screen
 *     makes a texture the presenter shows (scan-out), the bottom screen's also read back at 1x for Samus Returns'
 *     compositor. PWR_VK_NO_GPUCOPY=1 leaves the aliases out (the CPU copies then).
 *   - Textures are decoded on the CPU (pica_decode_level), cached by address, size, format and levels and checked against
 *     guest memory once per command list; texture packs' replacements are uploaded here, mipmapped by the driver.
 *   - Each command list's GPU time is measured (timer queries) for main.c's 2 s load line, as pica_vk.c's timestamps.
 * Its context shares the presenter's and is current on the GX worker: gsp.c runs every GX command there when presenting
 * with OpenGL (no small commands on the game's threads). The scan-out textures go between the two contexts under
 * scan_lock, each copy and each frame of the presenter's use followed by a fence the other side waits for.
 * Coordinates: OpenGL's window and texture rows run from row 0 as Vulkan's do, so targets, uploads and downloads are laid
 * out as pica_vk.c's; only the facing of triangles reads the other way round (glFrontFace(GL_CW) for Vulkan's
 * counter-clockwise), and the depth range is Vulkan's through glClipControl, else pica.vert's CLIP_Z. */
#include "pica_gl.h"
#include "gl_load.h"
#include "msr/msr.h"
#include <SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define R(i) pica_regs[i]
/* pica_vk.c's counters, which main.c prints every 2 s */
extern unsigned long long g_vk_draws, g_vk_batches, g_vk_uploads, g_vk_tex_uploads, g_vk_passes, g_vk_verts, g_vk_idx, g_vk_gpu_us;
int vgl_renderer_bind(void);   /* video_gl.c: this renderer's context current on the calling thread */
int vgl_compiler_bind(void);   /* video_gl.c: the program compiles' context current on the calling thread */
uint64_t pica_vsgen_key(void);   /* pica_vsgen.c */
int pica_vsgen_glsl(const char *clip_z, const char **text, size_t *len);
const char *pica_vsgen_build(void);
extern unsigned g_vsgen_compiled, g_vsgen_failed;
int rt_replace_file(const char *tmp, const char *dst);

static const char pica_vert[] =
#include "pica.vert.glsl.inc"
;
static const char pica_frag[] =
#include "pica.frag.glsl.inc"
;
static const char pica_vs_vert[] =
#include "pica_vs.vert.glsl.inc"
;

static int state = -1;   /* -1 not started, 0 off, 1 on */
static pthread_t owner;  /* the thread its context is current on (the GX worker) */
static int q_res = 1, q_aniso = 1, cur_scale = 1;
static float max_aniso = 1;
static volatile int reset_pending;   /* a save state was loaded (another thread): done at the next call on the GX worker */
static int gpu_vs;       /* vertex shaders on the GPU */
static int spec_on;      /* specialised fragment shaders */
static int preload_on = 1;   /* pwr_gl_programs.bin read at the start and written (--preload-shaders=off: neither) */
static const char *clip_z;   /* CLIP_Z(z, w)'s body in the vertex shaders: Vulkan's depth range or OpenGL's */
static GLuint vs_cpu_sh, vs_int_sh, fs_gen_sh[2];   /* pica.vert, pica_vs.vert, generic pica.frag (and without its depth write) */
static GLuint prog_cpu[2], prog_interp[2];   /* with pica.vert, with the interpreter: the generic fragment shader */
static GLuint vao, vao_gpu, white, fbo_a, fbo_b, lut_buf, lut_tex, prog_buf, prog_tex, attr_f32_tex, attr_u32_tex;
static uint32_t prog_version = UINT32_MAX, lut_version = UINT32_MAX;
static GLint ubo_align = 256;
static uint32_t batch_no = 1, list_no = 1, use_seq;
static int batch_open;   /* draws made since the last read-backs (pica_gl_flush) */
static int st_valid;     /* the draws' fixed-function state (last, below) is what OpenGL has (cleared when unknown) */
/* program binaries (OpenGL 4.1 or ARB_get_program_binary) */
static void (APIENTRY *get_program_binary)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
static void (APIENTRY *program_binary)(GLuint, GLenum, const void *, GLsizei);
static void (APIENTRY *program_parameteri)(GLuint, GLenum, GLint);

/* PWR_GL_PROF=1: where the GX thread's time goes in this renderer, every 2 s */
static double prof_ms[7]; static unsigned prof_n[7]; static int prof_on = -1;
static const char *const prof_names[7] = { "target uploads", "write-backs", "textures", "uploads", "state+draw", "draws", "flushes" };
static double prof_now(void) { return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }
#define PROF(i, stmt) do { if (prof_on) { double t0_ = prof_now(); stmt; prof_ms[i] += prof_now() - t0_; prof_n[i]++; } else { stmt; } } while (0)
static void prof_report(void) {
    static double last;
    const double now = prof_now();
    if (!last) last = now;
    if (now - last < 2000.0) return;
    char line[512]; int n = snprintf(line, sizeof line, "pica_gl: in %.0f ms:", now - last);
    for (int i = 0; i < 7; i++) n += snprintf(line + n, sizeof line - (size_t)n, " %s %.1f ms (%u)", prof_names[i], prof_ms[i], prof_n[i]);
    fprintf(stderr, "%s\n", line);
    memset(prof_ms, 0, sizeof prof_ms); memset(prof_n, 0, sizeof prof_n); last = now;
}

static atomic_int nlinked;   /* programs made (the stats line's "pipelines") */
int pica_gl_on(void) { return state == 1; }
int pica_gl_programs(void) { return state == 1 ? 4 + atomic_load(&nlinked) : 0; }
int pica_gl_gpu_vs(void) { return state == 1 && gpu_vs; }
void pica_gl_quality(int resolution, int aniso) { q_res = resolution < 1 ? 1 : resolution > 10 ? 10 : resolution; q_aniso = aniso < 1 ? 1 : aniso; }
void pica_gl_preload(int on) { preload_on = on; }

static uint64_t hash_mem(uint32_t addr, uint32_t size) {   /* pica_vk.c hash_mem_ */
    uint64_t h[4] = { 0x9E3779B97F4A7C15ull ^ size, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
    const uint8_t *p = g_mem + addr;
    uint32_t i = 0;
    for (; i + 32 <= size; i += 32)
        for (int l = 0; l < 4; l++) { uint64_t v; memcpy(&v, p + i + 8 * l, 8); h[l] = (h[l] ^ v) * 0x100000001B3ull; h[l] ^= h[l] >> 29; }
    uint64_t r = h[0] ^ (h[1] * 0x9E3779B97F4A7C15ull) ^ (h[2] * 0xC2B2AE3D27D4EB4Full) ^ (h[3] * 0x165667B19E3779F9ull);
    for (; i < size; i++) r = (r ^ p[i]) * 0x100000001B3ull;
    return r;
}
static uint64_t mix64(uint64_t h, uint64_t v) { h ^= v; h *= 0x100000001B3ull; h ^= h >> 31; return h; }

/* the CPU's own memory for uploads and downloads, grown as needed */
static void *scratch(size_t n) {
    static void *p; static size_t cap;
    if (n > cap) { free(p); cap = n + (n >> 2); p = malloc(cap); if (!p) cap = 0; }
    return p;
}

/* ---------------- bindings ----------------
   Texture units: 0-2 the draw's textures (with samplers), 3 the LUTs, 4 the vertex shader program, 5 and 6 the
   attributes (as vec4s and as words), 7 uploads. Only changes reach OpenGL. */
static int active_unit = -1; static GLuint unit_tex[3], unit_smp[3], bound_vao, bound_fbo;
static void activate(int u) { if (active_unit != u) { glActiveTexture(GL_TEXTURE0 + (GLenum)u); active_unit = u; } }
static void bind_scratch(GLuint t) { activate(7); glBindTexture(GL_TEXTURE_2D, t); }
static void bind_vao(GLuint v) { if (bound_vao != v) { glBindVertexArray(v); bound_vao = v; } }

/* A draw's data in one streamed buffer (its uniform blocks, attributes or vertices, indices), in its next free part. It
   stays bound to GL_COPY_WRITE_BUFFER, both vertex arrays take their indices (and the CPU-shaded vertices) from it, and
   the attributes' texture buffers are views of it. With OpenGL 4.4 (or ARB_buffer_storage) it is mapped for good and
   written in place, as a ring of DSEG segments: a fence when the draws leave one, waited for before they come back to it
   (a draw's data never spans two). Otherwise each draw's part is mapped without the driver's synchronisation (the part
   is new to the GPU), and the storage replaced once full (orphaned: draws still reading the old one keep it). Either
   way the driver neither copies the data nor checks the GPU's use of the buffer per draw. */
static GLuint dbuf; static GLsizeiptr dsize = 64 << 20, dused; static unsigned dgen;
static uint8_t *dmap;   /* the mapping for good, or NULL */
#define DSEG 4
static GLsync dseg_fence[DSEG]; static int dseg;
static void dbuf_orphan(void) {
    glBufferData(GL_COPY_WRITE_BUFFER, dsize, NULL, GL_STREAM_DRAW); dused = 0; dgen++;
    activate(5); glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, dbuf);   /* (the views take the new storage) */
    activate(6); glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, dbuf);
}
static GLsizeiptr dbuf_most(void) { return dmap ? dsize / DSEG : dsize; }   /* one draw's data at most */
/* n bytes from dused on (n <= dbuf_most()) */
static void dbuf_room(GLsizeiptr n) {
    if (!dmap) { if (dused + n > dsize) dbuf_orphan(); return; }
    const GLsizeiptr seg = dsize / DSEG;
    if (dused + n <= (GLsizeiptr)(dseg + 1) * seg) return;
    if (dseg_fence[dseg]) glDeleteSync(dseg_fence[dseg]);
    dseg_fence[dseg] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    dseg = (dseg + 1) % DSEG; dused = (GLsizeiptr)dseg * seg; dgen++;
    if (dseg_fence[dseg]) {   /* the draws that read it last time round */
        glClientWaitSync(dseg_fence[dseg], GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
        glDeleteSync(dseg_fence[dseg]); dseg_fence[dseg] = NULL;
    }
}
static GLintptr align_up(GLintptr v, GLintptr a) { return (v + a - 1) / a * a; }

/* ---------------- textures and framebuffers ---------------- */
static GLuint tex_new(uint32_t w, uint32_t h, int depth, int levels) {
    GLuint t; glGenTextures(1, &t);
    bind_scratch(t);
    for (int l = 0; l < levels; l++) {
        GLsizei lw = (GLsizei)(w >> l ? w >> l : 1), lh = (GLsizei)(h >> l ? h >> l : 1);
        if (depth) glTexImage2D(GL_TEXTURE_2D, l, GL_DEPTH32F_STENCIL8, lw, lh, 0, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, NULL);
        else glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

/* framebuffers by (colour, depth) texture pair, kept: render targets' pairs, and one colour texture each for copies and
   read-backs (attaching textures each time costs a validation) */
typedef struct { GLuint fbo, c, d; uint32_t used; } Fbo;
#define MAX_FBO 96
static Fbo fbos[MAX_FBO]; static uint32_t fbo_clock;
static GLuint fbo_for(GLuint c, GLuint d) {
    Fbo *f = NULL;
    for (int i = 0; i < MAX_FBO; i++) if (fbos[i].fbo && fbos[i].c == c && fbos[i].d == d) { fbos[i].used = ++fbo_clock; return fbos[i].fbo; }
    for (int i = 0; i < MAX_FBO && !f; i++) if (!fbos[i].fbo) f = &fbos[i];
    if (!f) { f = &fbos[0]; for (int i = 1; i < MAX_FBO; i++) if (fbos[i].used < f->used) f = &fbos[i]; }
    if (!f->fbo) glGenFramebuffers(1, &f->fbo);
    f->c = c; f->d = d; f->used = ++fbo_clock;
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo); bound_fbo = f->fbo;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "pica_gl: framebuffer incomplete (%#x)\n", st);
    return f->fbo;
}
static void fbos_forget(GLuint tex) {   /* a texture is going away: the framebuffers holding it too */
    for (int i = 0; i < MAX_FBO; i++)
        if (fbos[i].fbo && (fbos[i].c == tex || fbos[i].d == tex)) {
            if (bound_fbo == fbos[i].fbo) { glBindFramebuffer(GL_FRAMEBUFFER, 0); bound_fbo = 0; }
            glDeleteFramebuffers(1, &fbos[i].fbo); memset(&fbos[i], 0, sizeof fbos[i]);
        }
}
static void tex_delete(GLuint *t) {
    if (!*t) return;
    fbos_forget(*t);
    for (int u = 0; u < 3; u++) if (unit_tex[u] == *t) unit_tex[u] = 0;   /* (OpenGL unbinds it from this context's units) */
    glDeleteTextures(1, t); *t = 0;
}
/* fbo_a (read) and fbo_b (draw) for depth/stencil copies and clears */
static void attach_depth(GLenum target, GLuint fbo, GLuint tex) {
    glBindFramebuffer(target, fbo);
    glFramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(target, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, tex, 0);
    if (target == GL_READ_FRAMEBUFFER) glReadBuffer(GL_NONE); else glDrawBuffer(GL_NONE);
    bound_fbo = (GLuint)-1;
}

/* a draw's fixed-function state, set where it differs from the last draw's (copies and clears change some of it too) */
typedef struct {
    GLuint fbo, prog; GLint vp[4], sc[4];
    uint8_t blend, cmask, dt, dw, ste, cull; GLenum sfac[2], dfac[2], eq[2], dfn, sfn, sop[3]; GLint sref; GLuint scmask, swmask; float bc[4];
} DrawSetup;
static DrawSetup last;
static int scissor_on;
/* copies: no scissor, every colour channel written */
static void copy_state(void) {
    if (scissor_on) { glDisable(GL_SCISSOR_TEST); scissor_on = 0; }
    if (!st_valid || last.cmask != 15) { glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); last.cmask = 15; }
}
/* clears and depth copies: every channel, depth and stencil bit written too */
static void clear_state(void) {
    copy_state();
    glDepthMask(GL_TRUE); glStencilMask(0xFF); last.dw = 1; last.swmask = 0xFF;
}
static void blit_color(GLuint src, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLuint dst, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLenum filter) {
    copy_state();
    const GLuint rf = fbo_for(src, 0), df = fbo_for(dst, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rf); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, df); bound_fbo = (GLuint)-1;
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, filter);
}
static void read_from(GLuint tex) { const GLuint f = fbo_for(tex, 0); glBindFramebuffer(GL_READ_FRAMEBUFFER, f); bound_fbo = (GLuint)-1; }
static void clear_color(GLuint tex, const GLfloat c[4]) {
    clear_state();
    const GLuint f = fbo_for(tex, 0); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f); bound_fbo = (GLuint)-1;
    glClearBufferfv(GL_COLOR, 0, c);
}

/* ---------------- render targets ---------------- */
typedef struct { uint32_t addr, w, h; int fmt, depth; GLuint tex, down; uint64_t hash; int synced; uint32_t dirty_batch, checked_list, last_batch;
                 int pending_clear; uint8_t clear_px[4], clear_st; int sub, read_back; uint32_t use_seq;
                 GLuint pbo; GLsync wb_fence; int wb_pending; } Surf;   /* its read-back at 1x, not yet in guest memory while wb_pending */
/* as pica_vk.c's: clear_px RGBA8 or float depth, clear_st stencil; down: the 1x copy read back when the scale is above 1;
   sub: a depth window into a larger depth target (depth_window); read_back: a depth target the game copies */
#define MAX_SURF 64
static Surf surfs[MAX_SURF]; static int nsurf;
static Surf *cur_c, *cur_d;

static const uint8_t mx[8] = { 0, 1, 4, 5, 16, 17, 20, 21 }, my[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
static inline uint32_t fb_off(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t bpp) {
    y = h - 1 - y;
    return (mx[x & 7] + my[y & 7] + (x & ~7u) * 8 + (y & ~7u) * w) * bpp;
}
static uint32_t surf_bpp(const Surf *s) {
    static const uint32_t cb[5] = { 4, 3, 2, 2, 2 }, db[4] = { 2, 3, 3, 4 };
    return s->depth ? db[s->fmt & 3] : cb[s->fmt < 5 ? s->fmt : 0];
}
static uint32_t surf_size(const Surf *s) { return s->w * s->h * surf_bpp(s); }

static void surf_free(Surf *s) {
    tex_delete(&s->tex); tex_delete(&s->down);
    if (s->pbo) glDeleteBuffers(1, &s->pbo);
    if (s->wb_fence) glDeleteSync(s->wb_fence);
    s->pbo = 0; s->wb_fence = NULL; s->wb_pending = 0;
}
/* the texture cache (below) */
typedef struct { uint32_t addr, w, h; int fmt, levels; GLuint tex; uint64_t hash; uint32_t checked_list;
                 uint64_t rep_key; GLuint rep; int rep_extra, find_pending, glyph_sheet; unsigned glyph_ver; } Tex;   /* rep: a texture pack's */
static Tex *texs; static int ntex, cap_tex;
static void wb_complete(Surf *s);
static Surf *surf_get(uint32_t addr, uint32_t w, uint32_t h, int fmt, int depth) {
    for (int i = 0; i < nsurf; i++) {
        Surf *s = &surfs[i];
        if (s->addr == addr && s->w == w && s->h == h && s->fmt == fmt && s->depth == depth && !s->sub) return s;
    }
    Surf *s = NULL;
    if (nsurf < MAX_SURF) s = &surfs[nsurf++];
    else {
        for (int i = 0; i < nsurf; i++) {
            Surf *c = &surfs[i];
            if (c == cur_c || c == cur_d || c->dirty_batch == batch_no) continue;
            if (!s || c->last_batch < s->last_batch) s = c;
        }
        if (!s) return NULL;
        wb_complete(s);
        surf_free(s);
    }
    memset(s, 0, sizeof *s);
    s->addr = addr; s->w = w; s->h = h; s->fmt = fmt; s->depth = depth;
    s->tex = tex_new(w * (uint32_t)cur_scale, h * (uint32_t)cur_scale, depth, 1);
    return s;
}

/* one guest pixel -> RGBA8 (colour) or float (depth): pica_vk.c decode_px */
static void decode_px(const Surf *s, const uint8_t *p, uint8_t *o) {
    uint32_t v;
    if (s->depth) {
        float z = s->fmt == 0 ? (float)(p[0] | p[1] << 8) / 65535.f : (float)(p[0] | p[1] << 8 | p[2] << 16) / 16777215.f;
        memcpy(o, &z, 4); return;
    }
    switch (s->fmt) {
    case 0: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; break;
    case 1: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;
    case 2: v = p[0] | p[1] << 8; o[0] = (uint8_t)((v >> 11) * 255 / 31); o[1] = (uint8_t)((v >> 6 & 31) * 255 / 31); o[2] = (uint8_t)((v >> 1 & 31) * 255 / 31); o[3] = (v & 1) ? 255 : 0; break;
    case 3: v = p[0] | p[1] << 8; o[0] = (uint8_t)((v >> 11) * 255 / 31); o[1] = (uint8_t)((v >> 5 & 63) * 255 / 63); o[2] = (uint8_t)((v & 31) * 255 / 31); o[3] = 255; break;
    default: v = p[0] | p[1] << 8; o[0] = (uint8_t)((v >> 12) * 17); o[1] = (uint8_t)((v >> 8 & 15) * 17); o[2] = (uint8_t)((v >> 4 & 15) * 17); o[3] = (uint8_t)((v & 15) * 17); break;
    }
}

/* guest bytes -> the target's texture, each guest pixel S x S texels at the internal resolution S. Depth and stencil go
   up together (GL_FLOAT_32_UNSIGNED_INT_24_8_REV: the float, then a word with the stencil in its low byte). */
static void surf_upload(Surf *s) {
    const uint32_t w = s->w, h = s->h, bpp = surf_bpp(s), S = (uint32_t)cur_scale, W = w * S, H = h * S;
    bind_scratch(s->tex);
    if (s->depth) {
        uint32_t *d = scratch((size_t)W * H * 8);
        if (!d) return;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = g_mem + s->addr + fb_off(x, y, w, h, bpp);
            uint32_t z; decode_px(s, p, (uint8_t *)&z);
            const uint32_t st = s->fmt == 3 ? p[3] : 0;
            for (uint32_t j = 0; j < S; j++) for (uint32_t i = 0; i < S; i++) { size_t k = (size_t)(y * S + j) * W + x * S + i; d[2 * k] = z; d[2 * k + 1] = st; }
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)W, (GLsizei)H, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, d);
    } else {
        uint8_t *d = scratch((size_t)W * H * 4);
        if (!d) return;
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint8_t px[4]; decode_px(s, g_mem + s->addr + fb_off(x, y, w, h, bpp), px);
            for (uint32_t j = 0; j < S; j++) for (uint32_t i = 0; i < S; i++) memcpy(d + ((size_t)(y * S + j) * W + x * S + i) * 4, px, 4);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)W, (GLsizei)H, GL_RGBA, GL_UNSIGNED_BYTE, d);
    }
    s->hash = hash_mem(s->addr, surf_size(s)); s->synced = 1; g_vk_uploads++;
}

/* a colour target's pixels at 1x (rows from the bottom, RGBA8) -> the guest's tiled layout at dst (pica_vk.c surf_detile) */
static void detile_color(const Surf *s, const uint8_t *src, uint8_t *dst) {
    const uint32_t w = s->w, h = s->h, bpp = surf_bpp(s);
    if (!(w & 7) && !(h & 7) && (s->fmt == 0 || s->fmt == 1)) {   /* the eye and display targets: tiles in memory order */
        static uint8_t ox[64], oy[64];
        if (!oy[63]) for (int i = 0; i < 64; i++) { ox[i] = (uint8_t)((i & 1) | (i >> 1 & 2) | (i >> 2 & 4)); oy[i] = (uint8_t)((i >> 1 & 1) | (i >> 2 & 2) | (i >> 3 & 4)); }
        uint8_t *p = dst;
        for (uint32_t ty = 0; ty < h; ty += 8) for (uint32_t tx = 0; tx < w; tx += 8)
            for (int i = 0; i < 64; i++, p += bpp) {
                const uint8_t *c = src + ((size_t)(h - 1 - (ty + oy[i])) * w + tx + ox[i]) * 4;
                if (bpp == 4) { p[0] = c[3]; p[1] = c[2]; p[2] = c[1]; p[3] = c[0]; }
                else { p[0] = c[2]; p[1] = c[1]; p[2] = c[0]; }
            }
        return;
    }
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        const uint8_t *c = src + ((size_t)y * w + x) * 4;
        uint8_t *p = dst + fb_off(x, y, w, h, bpp);
        uint32_t r = c[0], g = c[1], b = c[2], a = c[3], v;
        switch (s->fmt) {
        case 0: p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = (uint8_t)g; p[3] = (uint8_t)r; break;
        case 1: p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r; break;
        case 2: v = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break;
        case 3: v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break;
        default: v = (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | (a >> 4); p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); break;
        }
    }
}

/* a colour target's read-back, at the end of a command list: filtered to 1x (linear), read into its pixel buffer, a
   fence after it; nothing waits (the caller flushes) */
static void wb_issue(Surf *s) {
    const uint32_t w = s->w, h = s->h, S = (uint32_t)cur_scale;
    GLuint from = s->tex;
    if (S > 1) {
        if (!s->down) s->down = tex_new(w, h, 0, 1);
        blit_color(s->tex, 0, 0, (GLint)(w * S), (GLint)(h * S), s->down, 0, 0, (GLint)w, (GLint)h, GL_LINEAR);
        from = s->down;
    }
    read_from(from);
    if (!s->pbo) { glGenBuffers(1, &s->pbo); glBindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo); glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, NULL, GL_STREAM_READ); }
    else glBindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (s->wb_fence) glDeleteSync(s->wb_fence);   /* an older read-back nobody needed: this one stands for it */
    s->wb_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    s->wb_pending = 1;
}
static int gl_current(void) { return SDL_GL_GetCurrentContext() != NULL; }
static void wb_drop(Surf *s) {   /* its read-back is not wanted any more (the guest's bytes are newer) */
    if (s->wb_fence && gl_current()) { glDeleteSync(s->wb_fence); s->wb_fence = NULL; }
    s->wb_pending = 0;
}
/* a read-back -> guest memory, waiting for it if it has not finished: on the GX worker, or the main thread (the
   presenter's context) with the worker idle */
static void wb_complete(Surf *s) {
    if (!s->wb_pending || !gl_current()) return;
    s->wb_pending = 0;
    if (s->wb_fence) { glClientWaitSync(s->wb_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull); glDeleteSync(s->wb_fence); s->wb_fence = NULL; }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo);
    const uint8_t *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)s->w * s->h * 4, GL_MAP_READ_BIT);
    if (src) detile_color(s, src, g_mem + s->addr);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (src) { s->hash = hash_mem(s->addr, surf_size(s)); s->synced = 1; }
}
static int aliases_touch(uint32_t start, uint32_t end);
static void aliases_writeback(uint32_t start, uint32_t end);
/* every read-back over [start, end) into guest memory (the aliases' too, unless for a target: except) */
static void wb_complete_range(uint32_t start, uint32_t end, const Surf *except) {
    if (!except && aliases_touch(start, end)) aliases_writeback(start, end);
    for (int i = 0; i < nsurf; i++) {
        Surf *s = &surfs[i];
        if (s->wb_pending && s != except && start < s->addr + surf_size(s) && s->addr < end) wb_complete(s);
    }
}
/* gsp.c: a GX command (or the CPU, at the VBlank tick: the main thread) reads or writes these guest bytes */
void pica_gl_sync_range(uint32_t start, uint32_t end) { if (state == 1 && !reset_pending) PROF(1, wb_complete_range(start, end, NULL)); }
void pica_gl_sync_all(void) { pica_gl_sync_range(0, 0xFFFFFFFFu); }
/* the VBlank tick (main thread, the GX worker idle): the read-backs that have finished land; nothing waits */
void pica_gl_sync_ready(void) {
    if (state != 1 || reset_pending || !gl_current()) return;
    for (int i = 0; i < nsurf; i++) {
        Surf *s = &surfs[i];
        if (!s->wb_pending || !s->wb_fence) continue;
        const GLenum st = glClientWaitSync(s->wb_fence, 0, 0);
        if (st == GL_ALREADY_SIGNALED || st == GL_CONDITION_SATISFIED) PROF(1, wb_complete(s));
    }
}
void pica_gl_alias_discard(uint32_t start, uint32_t end);
/* a MemoryFill over these bytes: read-backs wholly under it are not needed; the ones partly under it land first */
void pica_gl_discard_range(uint32_t start, uint32_t end) {
    if (state != 1) return;
    pica_gl_alias_discard(start, end);
    for (int i = 0; i < nsurf; i++) { Surf *s = &surfs[i]; if (s->wb_pending && s->addr >= start && s->addr + surf_size(s) <= end) wb_drop(s); }
    pica_gl_sync_range(start, end);
}

/* the target's texture -> guest bytes, waiting for the GPU: filtered to 1x first (nearest), read, tiled as the guest
   stores it (pica_vk.c surf_detile). Depth targets (colour ones go through wb_issue). */
static void surf_writeback(Surf *s) {
    const uint32_t w = s->w, h = s->h, bpp = surf_bpp(s), S = (uint32_t)cur_scale;
    GLuint from = s->tex;
    clear_state();
    if (S > 1) {
        if (!s->down) s->down = tex_new(w, h, 1, 1);
        attach_depth(GL_READ_FRAMEBUFFER, fbo_a, s->tex);
        attach_depth(GL_DRAW_FRAMEBUFFER, fbo_b, s->down);
        glBlitFramebuffer(0, 0, (GLint)(w * S), (GLint)(h * S), 0, 0, (GLint)w, (GLint)h, GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        from = s->down;
    }
    attach_depth(GL_READ_FRAMEBUFFER, fbo_a, from);
    uint8_t *dst = g_mem + s->addr;
    uint32_t *src = scratch((size_t)w * h * 8);
    if (!src) return;
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, src);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        size_t k = (size_t)y * w + x;
        float z; memcpy(&z, &src[2 * k], 4);
        z = z < 0 ? 0 : z > 1 ? 1 : z;
        uint8_t *q = dst + fb_off(x, y, w, h, bpp);
        if (s->fmt == 0) { uint32_t v = (uint32_t)(z * 65535.f + .5f); q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); }
        else { uint32_t v = (uint32_t)(z * 16777215.f + .5f); q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); q[2] = (uint8_t)(v >> 16); }
        if (s->fmt == 3) q[3] = (uint8_t)src[2 * k + 1];
    }
    s->hash = hash_mem(s->addr, surf_size(s)); s->synced = 1;
}

/* GX MemoryFill over exactly a known target: the target is cleared on the GPU instead of uploaded */
void pica_gl_filled(uint32_t start, uint32_t end) {
    if (state != 1) return;
    for (int i = 0; i < nsurf; i++) {
        Surf *s = &surfs[i];
        if (s->addr < start || s->addr + surf_size(s) > end) continue;
        decode_px(s, g_mem + s->addr, s->clear_px);
        s->clear_st = s->depth && s->fmt == 3 ? g_mem[s->addr + 3] : 0;
        s->pending_clear = 1; s->hash = hash_mem(s->addr, surf_size(s)); s->synced = 1;
    }
}
/* gsp.c: a GX operation reads these guest bytes: a depth target there is written back from now on */
void pica_gl_gx_reads(uint32_t start, uint32_t end) {
    if (state != 1) return;
    for (int i = 0; i < nsurf; i++) {
        Surf *s = &surfs[i];
        if (s->depth && !s->read_back && start < s->addr + surf_size(s) && s->addr < end) s->read_back = 1;
    }
}

/* guest bytes -> texture when they changed since this target was last seen, or a MemoryFill's clear */
static void surf_sync(Surf *t) {
    if (t->checked_list == list_no) return;
    t->checked_list = list_no;
    wb_complete_range(t->addr, t->addr + surf_size(t), t);   /* another target's (the same bytes, another size) on its way */
    if (!t->synced || hash_mem(t->addr, surf_size(t)) != t->hash) { wb_drop(t); PROF(0, surf_upload(t)); t->pending_clear = 0; }
    if (t->pending_clear) {
        t->pending_clear = 0;
        if (t->depth) {
            float z; memcpy(&z, t->clear_px, 4);
            clear_state(); attach_depth(GL_DRAW_FRAMEBUFFER, fbo_b, t->tex);
            glClearBufferfi(GL_DEPTH_STENCIL, 0, z, t->clear_st);
        } else {
            const GLfloat c[4] = { t->clear_px[0] / 255.f, t->clear_px[1] / 255.f, t->clear_px[2] / 255.f, t->clear_px[3] / 255.f };
            clear_color(t->tex, c);
        }
    }
}

/* A depth buffer inside a larger depth target (pica_vk.c depth_window: the game measures how much of the sun shows with
   an 8x8 depth buffer at the sun's tile of the scene's): the window's tiles are copied from the larger target's texture */
static Surf *depth_window(uint32_t daddr, uint32_t w, uint32_t h, int dfmt) {
    static const uint32_t db[4] = { 2, 3, 3, 4 };
    const uint32_t bpp = db[dfmt & 3], tile = 64 * bpp, size = w * h * bpp;
    if ((w & 7) || (h & 7)) return NULL;
    Surf *P = NULL, *W = NULL;
    for (int pass = 0; pass < 2; pass++) {
        P = NULL;
        for (int i = 0; i < nsurf; i++) {
            Surf *s = &surfs[i];
            if (!s->depth || s->sub || s->fmt != dfmt || (s->w <= w && s->h <= h) || daddr <= s->addr || daddr + size > s->addr + surf_size(s)) continue;
            if ((daddr - s->addr) % tile || (s->w & 7)) continue;
            if (!P || s->use_seq > P->use_seq) P = s;
        }
        if (!P || W) break;
        for (int i = 0; i < nsurf && !W; i++) if (surfs[i].sub && surfs[i].w == w && surfs[i].h == h && surfs[i].fmt == dfmt) W = &surfs[i];
        if (W) break;
        W = surf_get(daddr, w, h, dfmt, 1); if (!W) return NULL;
        W->sub = 1;
    }
    if (!P) return NULL;
    W->addr = daddr; W->checked_list = list_no; W->synced = 1; W->pending_clear = 0;
    surf_sync(P);
    clear_state();
    attach_depth(GL_READ_FRAMEBUFFER, fbo_a, P->tex);
    attach_depth(GL_DRAW_FRAMEBUFFER, fbo_b, W->tex);
    const uint32_t S = (uint32_t)cur_scale, tw = w / 8, ptw = P->w / 8, t0 = (daddr - P->addr) / tile;
    for (uint32_t k = 0; k < h / 8; k++) for (uint32_t j = 0; j < tw;) {   /* tile row k of the window, split where it wraps in P */
        const uint32_t T = t0 + k * tw + j, col = T % ptw, row = T / ptw, run = tw - j < ptw - col ? tw - j : ptw - col;
        const GLint sx = (GLint)(col * 8 * S), sy = (GLint)((P->h - 8 * (row + 1)) * S), dx = (GLint)(j * 8 * S), dy = (GLint)((h - 8 * (k + 1)) * S);
        glBlitFramebuffer(sx, sy, sx + (GLint)(run * 8 * S), sy + (GLint)(8 * S), dx, dy, dx + (GLint)(run * 8 * S), dy + (GLint)(8 * S),
                          GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        j += run;
    }
    return W;
}

static void aliases_drop(void);
static void scans_drop(void);
static void rescale(void) {   /* the internal resolution changed (between command lists): every target's bytes into guest memory first */
    for (int i = 0; i < nsurf; i++) { wb_complete(&surfs[i]); surf_free(&surfs[i]); }
    nsurf = 0; cur_c = cur_d = NULL;
    aliases_drop(); scans_drop();
    cur_scale = q_res;
    fprintf(stderr, "pica_gl: 3D resolution %dx\n", cur_scale);
}
static void gpu_time_drop(void);
static void do_reset(void) {   /* a save state was loaded: every target made again from guest memory */
    reset_pending = 0;
    gpu_time_drop();
    for (int i = 0; i < nsurf; i++) surf_free(&surfs[i]);
    nsurf = 0; cur_c = cur_d = NULL; batch_open = 0;
    aliases_drop(); scans_drop();
    for (int i = 0; i < ntex; i++) texs[i].checked_list = 0;
    prog_version = UINT32_MAX;
    list_no++;
}
/* a save state was loaded (main thread, the GX worker idle): no read-back may land over its memory; the GX worker drops
   the targets at its next command */
static void aliases_forget(void);
void pica_gl_state_reset(void) {
    if (state != 1) return;
    reset_pending = 1;
    for (int i = 0; i < nsurf; i++) surfs[i].wb_pending = 0;
    aliases_forget();
}

/* ---------------- GPU time ----------------
   Each command list's GPU work (from its first draw, uploads included, to its read-backs) is a timer query; finished
   ones add to g_vk_gpu_us (main.c's 2 s load line: "GPU drawing"), never waited for. */
#define NQUERY 64
static GLuint queries[NQUERY]; static int q_head, q_tail, q_open;   /* in flight: [q_head, q_tail); q_open: one is running */
static void gpu_time_harvest(void) {
    while (q_head != q_tail) {
        GLint ready = 0; glGetQueryObjectiv(queries[q_head], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) break;
        GLuint64 ns = 0; glGetQueryObjectui64v(queries[q_head], GL_QUERY_RESULT, &ns);
        g_vk_gpu_us += (unsigned long long)(ns / 1000);
        q_head = (q_head + 1) % NQUERY;
    }
}
static void gpu_time_begin(void) {
    if (q_open || (q_tail + 1) % NQUERY == q_head) return;   /* (all in flight: this list goes unmeasured) */
    glBeginQuery(GL_TIME_ELAPSED, queries[q_tail]); q_open = 1;
}
static void gpu_time_end(void) {
    if (!q_open) return;
    glEndQuery(GL_TIME_ELAPSED); q_open = 0; q_tail = (q_tail + 1) % NQUERY;
    gpu_time_harvest();
}
static void gpu_time_drop(void) { if (q_open) { glEndQuery(GL_TIME_ELAPSED); q_open = 0; } }

/* (colour, depth) from the framebuffer registers: the draw's targets, in sync with guest memory; 0 on success */
static int ensure_pass(void) {
    const uint32_t w = R(0x11E) & 0x7ff, h = ((R(0x11E) >> 12) & 0x3ff) + 1;
    const uint32_t caddr = pa_to_va((R(0x11D) & 0x0fffffff) << 3), daddr = pa_to_va((R(0x11C) & 0x0fffffff) << 3);
    int cfmt = (int)((R(0x117) >> 16) & 7); const int dfmt = (int)(R(0x116) & 3);
    if (cfmt > 4) cfmt = 0;
    if (!caddr || !w || !h) return -1;
    if (batch_open && cur_c && cur_c->addr == caddr && cur_c->w == w && cur_c->h == h && cur_c->fmt == cfmt &&
        ((!daddr && !cur_d) || (cur_d && cur_d->addr == daddr && cur_d->fmt == dfmt))) return 0;
    if (!batch_open) {
        if (q_res != cur_scale) rescale();
        gpu_time_begin();
    }
    batch_open = 1;
    cur_c = surf_get(caddr, w, h, cfmt, 0);
    cur_d = !daddr ? NULL : depth_window(daddr, w, h, dfmt);
    if (daddr && !cur_d) cur_d = surf_get(daddr, w, h, dfmt, 1);
    if (!cur_c || (daddr && !cur_d)) { cur_c = cur_d = NULL; return -1; }
    cur_c->last_batch = batch_no; cur_c->use_seq = ++use_seq;
    if (cur_d) { cur_d->last_batch = batch_no; cur_d->use_seq = ++use_seq; }
    surf_sync(cur_c);
    if (cur_d) surf_sync(cur_d);
    g_vk_passes++;
    return 0;
}

/* ---------------- textures ---------------- */
static int tex_lod_extra[3];   /* per unit, for sampler_for: the replacement's extra levels */
static int tex_packed[3];      /* per unit, for sampler_for: a texture pack's replacement is bound (smooth_packs) */
static uint32_t tex_level_size(const Tex *t, int l) { return (t->w >> l) * (t->h >> l) * (uint32_t)pica_tex_bpp[t->fmt] / 8; }

static void tex_upload(Tex *t) {
    uint32_t maddr = t->addr;
    bind_scratch(t->tex);
    for (int l = 0; l < t->levels; l++) {
        const uint32_t lw = t->w >> l, lh = t->h >> l;
        uint32_t *d = scratch((size_t)lw * lh * 4);
        if (!d) return;
        pica_decode_level(maddr, t->fmt, lw, lh, d);   /* texture row j = coordinate t, as pica_vk.c */
        if (l == 0 && t->glyph_sheet) msr_glyph_patch(t->glyph_sheet, (uint8_t *)d, lw, lh);   /* the device's face-button icons */
        glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, (GLsizei)lw, (GLsizei)lh, GL_RGBA, GL_UNSIGNED_BYTE, d);
        maddr += tex_level_size(t, l);
    }
    g_vk_tex_uploads++;
}

/* a texture pack's replacement once its PNG has loaded (msr/textures.cpp): level 0 from it, the levels below by the driver,
   down past the game's own by its scale */
static void tex_replace(Tex *t) {
    void *staged = NULL; const uint8_t *px; uint32_t rw, rh;
    const int r = msr_texture_take(t->rep_key, 0, &staged, &px, &rw, &rh);
    if (r < 0) { t->rep_key = 0; return; }
    if (r == 0 || !rw || !rh) return;
    int extra = 0; while (extra < 6 && (t->w << (extra + 1)) <= rw && (t->h << (extra + 1)) <= rh) extra++;
    int levels = 1; while (levels < t->levels + extra && (rw >> levels) >= 1 && (rh >> levels) >= 1) levels++;
    t->rep = tex_new(rw, rh, 0, levels);   /* (bound on the upload unit) */
    if (t->glyph_sheet) {   /* the device's face-button icons, at the pack's size */
        uint8_t *c = scratch((size_t)rw * rh * 4);
        if (c) { memcpy(c, px, (size_t)rw * rh * 4); msr_glyph_patch(t->glyph_sheet, c, rw, rh); px = c; }
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)rw, (GLsizei)rh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (levels > 1) glGenerateMipmap(GL_TEXTURE_2D);
    msr_texture_done(t->rep_key);
    t->rep_extra = extra;
    g_vk_tex_uploads++;
}

static GLuint alias_tex(uint32_t addr, uint32_t w, uint32_t h, int fmt, int levels);
/* the texture for a unit's registers; 0 with *flush set when a target drawn in this command list overlaps it without being
   it (its pixels must reach guest memory first: the caller reads back and tries again) */
static GLuint tex_get(int unit, int *flush) {
    tex_lod_extra[unit] = 0; tex_packed[unit] = 0;
    static const int dimr[3] = { 0x082, 0x092, 0x09A }, addrr[3] = { 0x085, 0x095, 0x09D }, fmtr[3] = { 0x08E, 0x096, 0x09E }, lodr[3] = { 0x084, 0x094, 0x09C };
    const uint32_t dim = R(dimr[unit]), h = dim & 0x7ff, w = (dim >> 16) & 0x7ff, addr = pa_to_va(R(addrr[unit]) << 3);
    const int fmt = (int)(R(fmtr[unit]) & 15);
    if (!w || !h || !addr || fmt > 13) return white;
    int levels = (int)(R(lodr[unit]) >> 16 & 15) + 1;
    while (levels > 1 && ((w >> (levels - 1)) < 8 || (h >> (levels - 1)) < 8)) levels--;   /* 8x8 is the smallest swizzle tile */
    uint32_t size = 0; for (int l = 0; l < levels; l++) size += (w >> l) * (h >> l) * (uint32_t)pica_tex_bpp[fmt] / 8;
    for (int i = 0; i < nsurf; i++) {   /* render to texture */
        Surf *s = &surfs[i];
        if (s->depth || s->dirty_batch != batch_no || !batch_open || addr >= s->addr + surf_size(s) || s->addr >= addr + size) continue;
        if (s->addr == addr && s->w == w && s->h == h && s->fmt == fmt && s != cur_c) return s->tex;   /* the whole target: its texture */
        *flush = 1; return 0;
    }
    { GLuint a = alias_tex(addr, w, h, fmt, levels); if (a) return a; }   /* a target copied here, at the internal resolution */
    wb_complete_range(addr, addr + size, NULL);   /* a target drawn in an earlier list, or a copy, whose bytes are the texture's */
    Tex *t = NULL;
    for (int i = 0; i < ntex; i++) if (texs[i].addr == addr && texs[i].w == w && texs[i].h == h && texs[i].fmt == fmt && texs[i].levels == levels) { t = &texs[i]; break; }
    int need = 0;
    if (!t) {
        if (ntex == cap_tex) { cap_tex = cap_tex ? cap_tex * 2 : 256; texs = realloc(texs, (size_t)cap_tex * sizeof *texs); }
        t = &texs[ntex++]; memset(t, 0, sizeof *t);
        t->addr = addr; t->w = w; t->h = h; t->fmt = fmt; t->levels = levels;
        t->tex = tex_new(w, h, 0, levels);
        t->hash = hash_mem(addr, size); t->checked_list = list_no; need = 1;
    } else if (t->checked_list != list_no) {
        t->checked_list = list_no;
        const uint64_t hh = hash_mem(addr, size);
        if (hh != t->hash) { t->hash = hh; need = 1; }
    }
    if (!need && t->glyph_sheet && t->glyph_ver != msr_glyph_version()) need = 1;   /* another device's face-button icons */
    if (need) {   /* new bytes: a texture pack may replace them (looked up by their hash, msr/textures.cpp) */
        t->glyph_sheet = msr_glyph_sheet(g_mem + addr, w, h, fmt); t->glyph_ver = msr_glyph_version();
        tex_delete(&t->rep);
        t->rep_extra = 0; t->find_pending = 0;
        t->rep_key = msr_textures_active() ? msr_texture_find(g_mem + addr, w, h, fmt, t->hash, &t->find_pending) : 0;
        tex_upload(t);
    } else if (t->find_pending)   /* a pack worker works out its hash: asked again (the game's texture shows meanwhile) */
        t->rep_key = msr_texture_find(g_mem + addr, w, h, fmt, t->hash, &t->find_pending);
    if (t->rep_key && !t->rep) tex_replace(t);
    if (t->rep) { tex_lod_extra[unit] = t->rep_extra; tex_packed[unit] = 1; return t->rep; }
    return t->tex;
}

/* sampler objects by the unit's filtering, wrapping and levels (pica_vk.c sampler_for); the border colour is the
   nearest of three, the shader returns the exact one outside [0, 1] */
static GLuint samplers[1 << 16];
static void (APIENTRY *sampler_parameterfv)(GLuint, GLenum, const GLfloat *);
static GLuint sampler_for(int unit) {
    static const int parr[3] = { 0x083, 0x093, 0x09B }, lodr[3] = { 0x084, 0x094, 0x09C };
    const uint32_t par = R(parr[unit]);
    const int ws = (int)((par >> 12) & 3), wt = (int)((par >> 8) & 3), smooth = tex_packed[unit] && msr_smooth_packs(), mip = (int)((par >> 24) & 1);
    const int mag = (int)((par >> 1) & 1) | smooth, min = (int)((par >> 2) & 1) | smooth;   /* #51 (pica_vk.c sampler_for) */
    int maxl = (int)(R(lodr[unit]) >> 16 & 15) + tex_lod_extra[unit]; if (maxl > 15) maxl = 15;
    const uint32_t bcol = R(unit == 0 ? 0x081 : unit == 1 ? 0x091 : 0x099);
    int bord = (bcol >> 24) < 128 ? 0 : ((bcol & 255) + (bcol >> 8 & 255) + (bcol >> 16 & 255)) >= 384 ? 2 : 1;
    if (ws != 1 && wt != 1) bord = 0;
    const int an = (min || mag) && q_aniso > 1 && max_aniso > 1 ? (q_aniso >= 16 ? 4 : q_aniso >= 8 ? 3 : q_aniso >= 4 ? 2 : 1) : 0;
    const int key = ws | wt << 2 | mag << 4 | min << 5 | mip << 6 | maxl << 7 | bord << 11 | an << 13;
    if (!samplers[key]) {
        static const GLenum wrap[4] = { GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER, GL_REPEAT, GL_MIRRORED_REPEAT };
        static const GLfloat border[3][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1, 1 } };
        GLuint s; glGenSamplers(1, &s);
        glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag ? GL_LINEAR : GL_NEAREST);
        glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, min ? (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : (mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST));
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)wrap[ws]);
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)wrap[wt]);
        glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, (GLfloat)maxl);
        if (sampler_parameterfv) sampler_parameterfv(s, GL_TEXTURE_BORDER_COLOR, border[bord]);
        if (an) glSamplerParameterf(s, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, fminf((float)(1 << an), max_aniso));
        samplers[key] = s;
    }
    return samplers[key];
}

/* ---------------- programs ----------------
   A program is a vertex shader (VS_CPU: pica.vert for CPU-shaded vertices; VS_INTERP: the interpreter; VS_GEN + i: the
   translation gens[i]) with pica.frag, specialised for a draw's configuration (pica_vk.c's 30 specialisation constants
   from draw_sc, as GLSL constants) or generic (SC_ON 0: its key is nd). The GX thread asks for the programs its draws
   want; they are compiled and linked on a thread of their own, in a context of their own, a fence saying when one can
   be used. Until then a draw takes the same vertex shader with the generic fragment shader if that is ready, else the
   interpreter's or pica.vert's with it (made at the start), as pica_vk.c falls back to its generic pipeline. The
   translations and the programs (with their binaries where the driver gives them: glGetProgramBinary) are kept in
   pwr_gl_programs.bin, read at the next start and made again (from the binaries: no compiling) before they are wanted. */
#define NSC 30
enum { VS_CPU, VS_INTERP, VS_GEN };
typedef struct { uint64_t key; char *text; GLuint sh; atomic_int state; } Gen;   /* state 0 not compiled yet, 1 compiled (sh), -1 failed */
typedef struct { uint32_t vs; int nd, spec; uint64_t fs; uint32_t sc[NSC]; GLuint prog; GLsync done; atomic_int state;
                 void *bin; GLsizei bin_len; GLenum bin_fmt; } Prog;   /* state 0 queued, 1 made (done: its fence), 2 ready, -1 failed */
#define MAX_GEN 4096
#define MAX_PROG 8192
static Gen gens[MAX_GEN]; static atomic_int ngen; static int gen_hash[2 * MAX_GEN];   /* open addressing: index + 1 */
static Prog progs[MAX_PROG]; static atomic_int nprog; static int prog_hash[2 * MAX_PROG];
static int jq[MAX_PROG + 1], jq_head, jq_tail;   /* the compile thread's queue of programs */
static pthread_mutex_t jq_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t jq_cond = PTHREAD_COND_INITIALIZER;
static atomic_int compiler_dead;   /* no context for the compiles: no program is asked for */
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;   /* the binaries, and writing the file */
static atomic_int cache_dirty;
static char cache_stamp[256];
#define CACHE_FILE "pwr_gl_programs.bin"
#define CACHE_MAGIC "MSRGLP01"

/* the shaders' first lines (shaders/pica.*): GLSL 3.30; the fragment shader's specialisation constants (all 0: generic) */
static void vs_header(char *h, size_t n) { snprintf(h, n, "#version 330 core\n#define UNROLL\n#define CLIP_Z(z, w) %s", clip_z); }
static void fs_header(char *h, size_t n, int nd, const uint32_t *sc) {
    static const char *const names[NSC] = { "SC_ON", "SC_SRC0", "SC_SRC1", "SC_SRC2", "SC_SRC3", "SC_SRC4", "SC_SRC5",
        "SC_OPS0", "SC_OPS1", "SC_OPS2", "SC_OPS3", "SC_OPS4", "SC_OPS5", "SC_CMB0", "SC_CMB1", "SC_CMB2", "SC_CMB3", "SC_CMB4", "SC_CMB5",
        "SC_SCL0", "SC_SCL1", "SC_SCL2", "SC_SCL3", "SC_SCL4", "SC_SCL5", "SC_BUFCFG", "SC_ALPHA", "SC_TEXEN", "SC_LIGHT", "SC_ZBUF" };
    int k = snprintf(h, n, "#version 330 core\n#define UNROLL\n%sconst uint", nd ? "#define NO_FRAG_DEPTH\n" : "");
    for (int i = 0; i < NSC && k > 0 && (size_t)k < n; i++) k += snprintf(h + k, n - (size_t)k, "%s %s = 0x%Xu", i ? "," : "", names[i], sc ? sc[i] : 0u);
    if (k > 0 && (size_t)k < n) snprintf(h + k, n - (size_t)k, ";");
}
/* a program's uniform blocks and samplers, on the same binding points and units for every program here */
static void setup_program(GLuint p) {
    GLuint b = glGetUniformBlockIndex(p, "Draw"); if (b != GL_INVALID_INDEX) glUniformBlockBinding(p, b, 0);
    b = glGetUniformBlockIndex(p, "VsDraw"); if (b != GL_INVALID_INDEX) glUniformBlockBinding(p, b, 1);
    glUseProgram(p);
    static const char *const units[7] = { "tex0", "tex1", "tex2", "lut", "prog_buf", "attr_buf", "raw_buf" };
    for (int i = 0; i < 7; i++) { GLint loc = glGetUniformLocation(p, units[i]); if (loc >= 0) glUniform1i(loc, i); }
}
/* the configuration pica.frag is specialised for (pica_vk.c draw_pkey's sc) */
static void draw_sc(uint32_t *sc) {
    static const int tb[6] = { 0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8 };
    sc[0] = 1;
    for (int i = 0; i < 6; i++) { sc[1 + i] = R(tb[i]) & 0x0FFF0FFF; sc[7 + i] = R(tb[i] + 1) & 0x777FFF; sc[13 + i] = R(tb[i] + 2) & 0x000F000F; sc[19 + i] = R(tb[i] + 4) & 0x00030003; }
    sc[25] = R(0x0E0) & 0x1FF07; sc[26] = R(0x104) & 0x71; sc[27] = R(0x080) & 0x2007;
    sc[28] = (R(0x08F) & 1) && !(R(0x1C6) & 1); sc[29] = R(0x06D) & 1;
}
static uint64_t spec_key(const uint32_t *sc, int nd) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)nd;
    for (int i = 0; i < NSC; i++) h = mix64(h, sc[i]);
    return h | 1ull << 63;   /* (generic keys are 0 and 1) */
}

/* a translation's index by its key; with text, one is added (on the GX thread) */
static int gen_find(uint64_t key, const char *text, size_t len) {
    uint32_t j = (uint32_t)(key ^ key >> 32) & (2 * MAX_GEN - 1);
    for (; gen_hash[j]; j = (j + 1) & (2 * MAX_GEN - 1)) if (gens[gen_hash[j] - 1].key == key) return gen_hash[j] - 1;
    const int i = atomic_load(&ngen);
    if (!text || i >= MAX_GEN) return -1;
    Gen *g = &gens[i];
    if (!(g->text = malloc(len + 1))) return -1;
    memcpy(g->text, text, len); g->text[len] = 0;
    g->key = key; g->sh = 0; atomic_store(&g->state, 0);
    gen_hash[j] = i + 1;
    atomic_store(&ngen, i + 1);
    return i;
}
static void compile_queue(int i) {
    static int started;
    pthread_mutex_lock(&jq_lock);
    if (!started) { extern void *pica_gl_compiler(void *); pthread_t th; started = 1; if (!pthread_create(&th, NULL, pica_gl_compiler, NULL)) pthread_detach(th); }
    jq[jq_tail] = i; jq_tail = (jq_tail + 1) % (MAX_PROG + 1);
    pthread_cond_signal(&jq_cond); pthread_mutex_unlock(&jq_lock);
}
/* a program's index (vertex shader, fragment key), added and queued when new (GX thread); -1 when the table is full */
static int prog_find(uint32_t vs, uint64_t fs, int nd, const uint32_t *sc, int add) {
    const uint64_t k = mix64(mix64(0x9E3779B97F4A7C15ull, vs), fs);
    uint32_t j = (uint32_t)(k ^ k >> 32) & (2 * MAX_PROG - 1);
    for (; prog_hash[j]; j = (j + 1) & (2 * MAX_PROG - 1)) { const Prog *p = &progs[prog_hash[j] - 1]; if (p->vs == vs && p->fs == fs) return prog_hash[j] - 1; }
    const int i = atomic_load(&nprog);
    if (!add || i >= MAX_PROG || atomic_load(&compiler_dead)) return -1;
    Prog *p = &progs[i];
    p->vs = vs; p->fs = fs; p->nd = nd; p->spec = sc != NULL; if (sc) memcpy(p->sc, sc, sizeof p->sc); else memset(p->sc, 0, sizeof p->sc);
    p->prog = 0; p->done = NULL; p->bin = NULL; p->bin_len = 0; p->bin_fmt = 0; atomic_store(&p->state, 0);
    prog_hash[j] = i + 1;
    atomic_store(&nprog, i + 1);
    return i;
}
/* the program for (vertex shader, fragment key) if it can be used now; asked for when new */
static GLuint prog_ready(uint32_t vs, uint64_t fs, int nd, const uint32_t *sc) {
    static uint32_t last_vs = UINT32_MAX; static uint64_t last_fs; static int last_i = -1;   /* consecutive draws mostly share one */
    int i;
    if (last_i >= 0 && last_vs == vs && last_fs == fs) i = last_i;
    else {
        const int n = atomic_load(&nprog);
        i = prog_find(vs, fs, nd, sc, 1);
        if (i < 0) return 0;
        if (i == n) compile_queue(i);   /* new */
        last_vs = vs; last_fs = fs; last_i = i;
    }
    Prog *p = &progs[i];
    int st = atomic_load(&p->state);
    if (st == 1) {   /* made: usable here once its fence has signalled */
        const GLenum r = glClientWaitSync(p->done, 0, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) return 0;
        glDeleteSync(p->done); p->done = NULL; atomic_store(&p->state, 2); st = 2;
    }
    return st == 2 ? p->prog : 0;
}
/* the current registers' translation (made when new), -1 when there is none (the interpreter then) */
static int gen_current(void) {
    static int off = -1; if (off < 0) off = getenv("PWR_VK_NO_VSGEN") != NULL;
    if (off || atomic_load(&compiler_dead)) return -1;
    const uint64_t key = pica_vsgen_key();
    static uint64_t last_key; static int last = -2;
    if (last != -2 && last_key == key) return last >= 0 && atomic_load(&gens[last].state) >= 0 ? last : -1;
    int i = gen_find(key, NULL, 0);
    if (i < 0) {
        const char *t; size_t n;
        if (pica_vsgen_glsl(clip_z, &t, &n)) { g_vsgen_failed++; i = -1; }
        else {
            static int dump = -1; if (dump < 0) dump = getenv("PWR_GL_DUMPVS") != NULL;   /* each translation to vs_<key>.vert */
            if (dump) { char fn[48]; snprintf(fn, sizeof fn, "vs_%016llx.vert", (unsigned long long)key); FILE *f = fopen(fn, "wb"); if (f) { fwrite(t, 1, n, f); fclose(f); } }
            i = gen_find(key, t, n);
        }
    }
    last_key = key; last = i;
    return i >= 0 && atomic_load(&gens[i].state) >= 0 ? i : -1;
}

/* writes pwr_gl_programs.bin: the translations, then every program made (its shaders' keys and its binary) */
static void cache_save(void) {
    if (!preload_on) return;
    pthread_mutex_lock(&save_lock);
    atomic_store(&cache_dirty, 0);
    FILE *f = fopen(CACHE_FILE ".tmp", "wb");
    if (!f) { pthread_mutex_unlock(&save_lock); return; }
    const int ng = atomic_load(&ngen), np = atomic_load(&nprog);
    uint32_t n = 0;
    fwrite(CACHE_MAGIC, 1, 8, f); fwrite(cache_stamp, 1, sizeof cache_stamp, f);
    for (int i = 0; i < ng; i++) n += gens[i].text && atomic_load(&gens[i].state) >= 0;
    fwrite(&n, 4, 1, f);
    for (int i = 0; i < ng; i++) {
        const Gen *g = &gens[i];
        if (!g->text || atomic_load(&gens[i].state) < 0) continue;
        const uint32_t len = (uint32_t)strlen(g->text);
        fwrite(&g->key, 8, 1, f); fwrite(&len, 4, 1, f); fwrite(g->text, 1, len, f);
    }
    n = 0;
    for (int i = 0; i < np; i++) n += atomic_load(&progs[i].state) >= 1;
    fwrite(&n, 4, 1, f);
    for (int i = 0; i < np; i++) {
        Prog *p = &progs[i];
        if (atomic_load(&p->state) < 1) continue;
        const uint32_t kind = p->vs >= VS_GEN ? VS_GEN : p->vs, nd = (uint32_t)p->nd, spec = (uint32_t)p->spec, fmt = (uint32_t)p->bin_fmt, len = p->bin ? (uint32_t)p->bin_len : 0;
        const uint64_t vkey = p->vs >= VS_GEN ? gens[p->vs - VS_GEN].key : 0;
        fwrite(&kind, 4, 1, f); fwrite(&vkey, 8, 1, f); fwrite(&p->fs, 8, 1, f); fwrite(&nd, 4, 1, f); fwrite(&spec, 4, 1, f);
        fwrite(p->sc, 4, NSC, f); fwrite(&fmt, 4, 1, f); fwrite(&len, 4, 1, f); if (len) fwrite(p->bin, 1, len, f);
    }
    const int ok = !ferror(f); fclose(f);
    if (ok) rt_replace_file(CACHE_FILE ".tmp", CACHE_FILE); else remove(CACHE_FILE ".tmp");
    pthread_mutex_unlock(&save_lock);
}
/* at exit (main thread): what the compiles made since the last save */
void pica_gl_saves_wait(void) { if (state == 1 && atomic_load(&cache_dirty)) cache_save(); }
/* at the start (GX thread): the last run's translations and programs, queued to be made again now */
static void cache_load(void) {
    FILE *f = fopen(CACHE_FILE, "rb");
    if (!f) return;
    char magic[8], stamp[sizeof cache_stamp]; uint32_t n = 0; int ok = 1, nv = 0, np = 0, nb = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, CACHE_MAGIC, 8) || fread(stamp, 1, sizeof stamp, f) != sizeof stamp || memcmp(stamp, cache_stamp, sizeof stamp)) {
        fprintf(stderr, "pica_gl: " CACHE_FILE " is from another build or graphics driver: programs are made when needed and saved again\n");
        fclose(f); return;
    }
    const double t0 = prof_now();
    if (fread(&n, 4, 1, f) != 1 || n > MAX_GEN) ok = 0;
    for (uint32_t i = 0; ok && i < n; i++) {
        uint64_t key; uint32_t len;
        if (fread(&key, 8, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > (1u << 22)) { ok = 0; break; }
        char *t = malloc(len + 1);
        if (!t || fread(t, 1, len, f) != len) { free(t); ok = 0; break; }
        if (gen_find(key, t, len) >= 0) nv++;
        free(t);
    }
    if (ok && (fread(&n, 4, 1, f) != 1 || n > MAX_PROG)) ok = 0;
    for (uint32_t i = 0; ok && i < n; i++) {
        uint32_t kind, nd, spec, fmt, len, sc[NSC]; uint64_t vkey, fs;
        if (fread(&kind, 4, 1, f) != 1 || fread(&vkey, 8, 1, f) != 1 || fread(&fs, 8, 1, f) != 1 || fread(&nd, 4, 1, f) != 1 || fread(&spec, 4, 1, f) != 1 ||
            fread(sc, 4, NSC, f) != NSC || fread(&fmt, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > (1u << 26)) { ok = 0; break; }
        void *bin = NULL;
        if (len && (!(bin = malloc(len)) || fread(bin, 1, len, f) != len)) { free(bin); ok = 0; break; }
        uint32_t vs = kind;
        if (kind == VS_GEN) { const int g = gen_find(vkey, NULL, 0); if (g < 0) { free(bin); continue; } vs = VS_GEN + (uint32_t)g; }
        else if (kind != VS_CPU && kind != VS_INTERP) { free(bin); continue; }
        if ((kind == VS_CPU) == (gpu_vs != 0)) { free(bin); continue; }   /* the other mode's: left out (and out of the next save) */
        const int j = prog_find(vs, fs, (int)nd & 1, spec ? sc : NULL, 1);
        if (j < 0 || j != atomic_load(&nprog) - 1) { free(bin); continue; }
        Prog *p = &progs[j];
        p->bin = bin; p->bin_len = (GLsizei)len; p->bin_fmt = (GLenum)fmt;
        compile_queue(j); np++; nb += bin != NULL;
    }
    fclose(f);
    fprintf(stderr, "pica_gl: " CACHE_FILE ": %d translations, %d programs (%d binaries) queued (%.0f ms)%s\n", nv, np, nb, prof_now() - t0, ok ? "" : ", the rest unreadable");
}

/* the compile thread: makes each queued program, from its binary if it has one (no compiling), else from its shaders
   (compiled once each, kept for the next programs), and saves the cache when idle (at most every 5 s) */
void *pica_gl_compiler(void *arg) {
    (void)arg;
    if (vgl_compiler_bind()) {
        fprintf(stderr, "pica_gl: no context for the program compiles: the generic programs stay\n");
        atomic_store(&compiler_dead, 1); return NULL;
    }
    k_background_thread();   /* draws use the generic programs until theirs are ready */
    static int trace = -1; if (trace < 0) trace = getenv("PWR_VSGEN_TRACE") != NULL;
    static struct { uint64_t key; GLuint sh; } fsc[4096]; static int nfsc;   /* fragment shaders by key (this thread's) */
    const int binaries = get_program_binary && program_binary && program_parameteri;
    double last_save = prof_now();
    for (;;) {
        pthread_mutex_lock(&jq_lock);
        while (jq_head == jq_tail) {
            if (atomic_load(&cache_dirty) && prof_now() - last_save >= 5000.0) {   /* idle: the cache saved */
                pthread_mutex_unlock(&jq_lock); cache_save(); last_save = prof_now(); pthread_mutex_lock(&jq_lock); continue;
            }
            if (atomic_load(&cache_dirty)) {   /* wait out the 5 s, or a new job */
                struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
                pthread_cond_timedwait(&jq_cond, &jq_lock, &ts);
            } else pthread_cond_wait(&jq_cond, &jq_lock);
        }
        const int i = jq[jq_head]; jq_head = (jq_head + 1) % (MAX_PROG + 1);
        pthread_mutex_unlock(&jq_lock);
        Prog *p = &progs[i];
        const double t0 = prof_now();
        GLuint prog = 0; int from_bin = 0;
        if (p->bin && program_binary) {
            prog = glCreateProgram();
            program_binary(prog, p->bin_fmt, p->bin, p->bin_len);
            GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (ok) from_bin = 1; else { glDeleteProgram(prog); prog = 0; }   /* (another driver's: made from its shaders) */
        }
        if (!prog) {
            GLuint vs = p->vs == VS_CPU ? vs_cpu_sh : p->vs == VS_INTERP ? vs_int_sh : 0;
            if (p->vs >= VS_GEN) {   /* a translation: compiled with its first program */
                Gen *g = &gens[p->vs - VS_GEN];
                if (atomic_load(&g->state) == 0) {
                    g->sh = msr_gl_shader("translated", GL_VERTEX_SHADER, NULL, g->text);
                    if (!g->sh && getenv("PWR_GL_DEBUG")) fprintf(stderr, "%s\n", g->text);
                    if (g->sh) g_vsgen_compiled++; else g_vsgen_failed++;
                    atomic_store(&g->state, g->sh ? 1 : -1);
                }
                vs = g->sh;
            }
            GLuint fs = !p->spec ? fs_gen_sh[p->nd] : 0;
            if (p->spec) {
                int k = 0; while (k < nfsc && fsc[k].key != p->fs) k++;
                if (k < nfsc) fs = fsc[k].sh;
                else {
                    char header[2048]; fs_header(header, sizeof header, p->nd, p->sc);
                    fs = msr_gl_shader("pica (specialised)", GL_FRAGMENT_SHADER, header, pica_frag);
                    if (fs && nfsc < (int)(sizeof fsc / sizeof *fsc)) { fsc[nfsc].key = p->fs; fsc[nfsc].sh = fs; nfsc++; }
                }
            }
            if (vs && fs) {
                prog = glCreateProgram();
                glAttachShader(prog, vs); glAttachShader(prog, fs);
                if (binaries) program_parameteri(prog, 0x8257 /* GL_PROGRAM_BINARY_RETRIEVABLE_HINT */, GL_TRUE);
                glLinkProgram(prog);
                GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
                if (!ok) {
                    char log[1024] = ""; glGetProgramInfoLog(prog, sizeof log, NULL, log);
                    fprintf(stderr, "pica_gl: program (vertex %u, fragment %016llx) failed: %s\n", p->vs, (unsigned long long)p->fs, log);
                    glDeleteProgram(prog); prog = 0;
                } else if (binaries) {   /* kept for the next start */
                    GLint len = 0; glGetProgramiv(prog, 0x8741 /* GL_PROGRAM_BINARY_LENGTH */, &len);
                    void *bin = len > 0 ? malloc((size_t)len) : NULL; GLenum fmt = 0; GLsizei got = 0;
                    if (bin) get_program_binary(prog, len, &got, &fmt, bin);
                    pthread_mutex_lock(&save_lock);
                    free(p->bin); p->bin = got > 0 ? bin : NULL; p->bin_len = got; p->bin_fmt = fmt;
                    pthread_mutex_unlock(&save_lock);
                    if (got <= 0) free(bin);
                }
            }
        }
        if (prog) {
            setup_program(prog);
            p->prog = prog; p->done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); glFlush();
            atomic_fetch_add(&nlinked, 1);
            if (!from_bin) atomic_store(&cache_dirty, 1);
            atomic_store(&p->state, 1);
        } else atomic_store(&p->state, -1);
        if (trace) fprintf(stderr, "pica_gl: program %d (vertex %u, %s fragment) %s in %.1f ms\n", i, p->vs, p->spec ? "specialised" : "generic",
                           !prog ? "failed" : from_bin ? "loaded" : "made", prof_now() - t0);
    }
    return NULL;
}

/* ---------------- start ---------------- */
static void APIENTRY gl_debug(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *user) {
    (void)source; (void)id; (void)length; (void)user;
    if (severity != 0x826B /* notification */) fprintf(stderr, "pica_gl: OpenGL says (type %#x): %s\n", type, message);
}

int pica_gl_init(void) {
    if (state >= 0) return state == 1 ? 0 : -1;
    state = 0;
    if (vgl_renderer_bind()) { fprintf(stderr, "pica_gl: no context for the renderer\n"); return -1; }
    owner = pthread_self();
    prof_on = getenv("PWR_GL_PROF") != NULL;
    if (getenv("PWR_GL_DEBUG")) {
        typedef void (APIENTRY *Callback)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
        void (APIENTRY *set)(Callback, const void *) = (void (APIENTRY *)(Callback, const void *))SDL_GL_GetProcAddress("glDebugMessageCallback");
        if (set) { glEnable(0x92E0); glEnable(0x8242); set(gl_debug, NULL); }
    }
    /* Vulkan's depth range where the driver has glClipControl (4.5, or ARB_clip_control): z/w is the depth as it is */
    void (APIENTRY *clip_control)(GLenum, GLenum) = NULL;
    if (msr_gl_version() >= 45 || msr_gl_has("GL_ARB_clip_control"))
        clip_control = (void (APIENTRY *)(GLenum, GLenum))SDL_GL_GetProcAddress("glClipControl");
    if (clip_control) clip_control(0x8CA1 /* GL_LOWER_LEFT */, 0x935F /* GL_ZERO_TO_ONE */);
    if (msr_gl_version() >= 46 || msr_gl_has("GL_EXT_texture_filter_anisotropic") || msr_gl_has("GL_ARB_texture_filter_anisotropic")) {
        GLint most = 1; glGetIntegerv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &most); max_aniso = most > 1 ? (float)most : 1;
    }
    sampler_parameterfv = (void (APIENTRY *)(GLuint, GLenum, const GLfloat *))SDL_GL_GetProcAddress("glSamplerParameterfv");
    if (msr_gl_version() >= 41 || msr_gl_has("GL_ARB_get_program_binary")) {
        GLint formats = 0; glGetIntegerv(0x87FE /* GL_NUM_PROGRAM_BINARY_FORMATS */, &formats);
        if (formats > 0) {
            get_program_binary = (void (APIENTRY *)(GLuint, GLsizei, GLsizei *, GLenum *, void *))SDL_GL_GetProcAddress("glGetProgramBinary");
            program_binary = (void (APIENTRY *)(GLuint, GLenum, const void *, GLsizei))SDL_GL_GetProcAddress("glProgramBinary");
            program_parameteri = (void (APIENTRY *)(GLuint, GLenum, GLint))SDL_GL_GetProcAddress("glProgramParameteri");
        }
    }
    clip_z = clip_control ? "(z)" : "(2.0 * (z) - (w))";
    spec_on = getenv("PWR_VK_NOSPEC") == NULL;
    {   /* the shaders every program shares (kept for the compile thread's), and the programs with the generic fragment shader */
        char header[2048];
        for (int nd = 0; nd < 2; nd++) {
            fs_header(header, sizeof header, nd, NULL);
            if (!(fs_gen_sh[nd] = msr_gl_shader(nd ? "pica (no depth write)" : "pica", GL_FRAGMENT_SHADER, header, pica_frag))) return -1;
        }
        vs_header(header, sizeof header);
        vs_cpu_sh = msr_gl_shader("pica", GL_VERTEX_SHADER, header, pica_vert);
        vs_int_sh = msr_gl_shader("pica_vs", GL_VERTEX_SHADER, header, pica_vs_vert);
        if (!vs_cpu_sh) return -1;
        for (int nd = 0; nd < 2; nd++) {
            if (!(prog_cpu[nd] = msr_gl_link("pica", vs_cpu_sh, fs_gen_sh[nd]))) return -1;
            setup_program(prog_cpu[nd]);
            if (vs_int_sh && (prog_interp[nd] = msr_gl_link("pica_vs", vs_int_sh, fs_gen_sh[nd]))) setup_program(prog_interp[nd]);
        }
        gpu_vs = prog_interp[0] && prog_interp[1] && !getenv("PWR_GL_CPU_VS");
    }
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_align); if (ubo_align < 16) ubo_align = 16;
    {   /* the draws' buffer: within the driver's texel count for its R32UI view */
        GLint most = 0; glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &most);
        if (most > 0 && (GLint64)most * 4 < (GLint64)dsize) dsize = (GLsizeiptr)most * 4 & ~(GLsizeiptr)255;
    }
    glGenBuffers(1, &dbuf); glBindBuffer(GL_COPY_WRITE_BUFFER, dbuf);
    {   /* mapped for good where the driver can (OpenGL 4.4, ARB_buffer_storage) */
        void (APIENTRY *buffer_storage)(GLenum, GLsizeiptr, const void *, GLbitfield) = NULL;
        if ((msr_gl_version() >= 44 || msr_gl_has("GL_ARB_buffer_storage")) && !getenv("PWR_GL_NO_PERSISTENT"))
            buffer_storage = (void (APIENTRY *)(GLenum, GLsizeiptr, const void *, GLbitfield))SDL_GL_GetProcAddress("glBufferStorage");
        const GLbitfield flags = 0x0002 /* GL_MAP_WRITE_BIT */ | 0x0040 /* GL_MAP_PERSISTENT_BIT */ | 0x0080 /* GL_MAP_COHERENT_BIT */;
        if (buffer_storage) {
            buffer_storage(GL_COPY_WRITE_BUFFER, dsize, NULL, flags);
            if (!glGetError()) dmap = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, dsize, flags);
            if (!dmap) {   /* (the storage is immutable: a new buffer for the other way) */
                glDeleteBuffers(1, &dbuf); glGenBuffers(1, &dbuf); glBindBuffer(GL_COPY_WRITE_BUFFER, dbuf);
            }
        }
        if (!dmap) glBufferData(GL_COPY_WRITE_BUFFER, dsize, NULL, GL_STREAM_DRAW);
    }
    /* the CPU-shaded vertex (pica_int.h Vtx), as pica_vk.c's pipeline reads it; both vertex arrays' indices from dbuf */
    glGenVertexArrays(1, &vao); bind_vao(vao);
    glBindBuffer(GL_ARRAY_BUFFER, dbuf); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dbuf);
    static const struct { GLint n; size_t off; } va[6] = { { 4, offsetof(Vtx, pos) }, { 4, offsetof(Vtx, col) }, { 4, offsetof(Vtx, uv) },
                                                          { 3, offsetof(Vtx, uv[2]) }, { 4, offsetof(Vtx, quat) }, { 3, offsetof(Vtx, view) } };
    for (GLuint i = 0; i < 6; i++) { glEnableVertexAttribArray(i); glVertexAttribPointer(i, va[i].n, GL_FLOAT, GL_FALSE, sizeof(Vtx), (const void *)va[i].off); }
    glGenVertexArrays(1, &vao_gpu); bind_vao(vao_gpu); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, dbuf);   /* no attributes */
    /* the lighting and fog LUTs on unit 3 (pica.frag's lut); the vertex shader program (pica_vs_prog: 4096 words, then
       128 swizzles) on unit 4; the attributes as vec4s (the interpreter) on unit 5 and as words (the translations' raw
       bytes) on unit 6: texture buffers */
    glGenBuffers(1, &lut_buf); glBindBuffer(GL_TEXTURE_BUFFER, lut_buf); glBufferData(GL_TEXTURE_BUFFER, sizeof light_lut + sizeof fog_lut, NULL, GL_DYNAMIC_DRAW);
    glGenTextures(1, &lut_tex); activate(3); glBindTexture(GL_TEXTURE_BUFFER, lut_tex); glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, lut_buf);
    glGenBuffers(1, &prog_buf); glBindBuffer(GL_TEXTURE_BUFFER, prog_buf); glBufferData(GL_TEXTURE_BUFFER, sizeof pica_vs_prog, NULL, GL_DYNAMIC_DRAW);
    glGenTextures(1, &prog_tex); activate(4); glBindTexture(GL_TEXTURE_BUFFER, prog_tex); glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, prog_buf);
    glGenTextures(1, &attr_f32_tex); activate(5); glBindTexture(GL_TEXTURE_BUFFER, attr_f32_tex); glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, dbuf);
    glGenTextures(1, &attr_u32_tex); activate(6); glBindTexture(GL_TEXTURE_BUFFER, attr_u32_tex); glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, dbuf);
    {   /* a white texel for units that are off */
        static const uint8_t px[4] = { 255, 255, 255, 255 };
        white = tex_new(1, 1, 0, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    glGenFramebuffers(1, &fbo_a); glGenFramebuffers(1, &fbo_b);
    glGenQueries(NQUERY, queries);
    glFrontFace(GL_CW);   /* Vulkan's counter-clockwise, as its sign convention for the area reads row-down coordinates */
    GLenum e = glGetError();
    if (e) { fprintf(stderr, "pica_gl: OpenGL error %#x starting\n", e); return -1; }
    glFinish();   /* (the shaders above are the compile thread's too) */
    {   /* what the program cache must match: the generators and shaders, the driver */
        uint64_t h = 0xcbf29ce484222325ull;
        const char *srcs[3] = { pica_vert, pica_frag, pica_vs_vert };
        for (int s = 0; s < 3; s++) for (const char *c = srcs[s]; *c; c++) h = mix64(h, (uint8_t)*c);
        memset(cache_stamp, 0, sizeof cache_stamp);
        snprintf(cache_stamp, sizeof cache_stamp, "%s|%016llx|%s|%s|%s|%s", pica_vsgen_build(), (unsigned long long)h, clip_z,
                 (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    }
    state = 1;
    fprintf(stderr, "pica_gl: hardware renderer on (OpenGL %d.%d, %s depth range, vertex shaders on the %s, %s fragment shaders, program binaries %s, draw data %s)\n",
            msr_gl_version() / 10, msr_gl_version() % 10, clip_control ? "Vulkan's" : "OpenGL's", gpu_vs ? "GPU" : "CPU",
            spec_on ? "specialised" : "generic", get_program_binary && program_binary && program_parameteri ? "kept" : "unavailable",
            dmap ? "mapped for good" : "mapped per draw");
    if (preload_on && !getenv("PWR_NO_PRELOAD")) cache_load();
    return 0;
}

/* this thread has the context (the GX worker); a save state loaded meanwhile is taken in */
static int gl_here(void) {
    if (state != 1 || !pthread_equal(owner, pthread_self())) {
        static int told; if (state == 1 && !told) { told = 1; fprintf(stderr, "pica_gl: called off the GX worker (ignored)\n"); }
        return 0;
    }
    if (reset_pending) do_reset();
    return 1;
}

/* ---------------- read-backs at the end of a command list ---------------- */
static void pica_gl_flush_(void);
void pica_gl_flush(void) {
    PROF(6, pica_gl_flush_());
    if (prof_on) prof_report();
}
static void pica_gl_flush_(void) {
    if (!gl_here() || !batch_open) return;
    int issued = 0;
    for (int i = 0; i < nsurf; i++) {
        Surf *s = &surfs[i];
        if (s->dirty_batch != batch_no) continue;
        if (!s->depth) { wb_issue(s); issued = 1; }
        else if (s->read_back && !s->sub) surf_writeback(s);
    }
    gpu_time_end();
    if (issued) glFlush();   /* (another context may wait for these fences) */
    batch_no++; list_no++; batch_open = 0; cur_c = cur_d = NULL;
    g_vk_batches++;
}

/* ---------------- render targets copied into textures, on the GPU (pica_vk.c's aliases) ----------------
   Samus Returns renders its scene into a 240x400 target and TextureCopies it into a 256x512 texture its final composite
   samples, and halves that into the bloom and light-shaft chains with DisplayTransfers. These copies are made here
   between textures at the internal resolution (rows as pica_vk.c lays them out), and tex_get gives such a texture in
   place of the copied bytes while guest memory there is unchanged. gsp.c skips its CPU copy (1); the copy's bytes are
   read back at 1x without waiting and land when something reads them (alias_writeback: the part the copy wrote). */
typedef struct { uint32_t addr, w, h, fmt, span, checked_list, cw, ch; uint64_t hash; int valid, deferred; GLuint tex, down, pbo; GLsync fence; } Alias;
/* deferred: its guest bytes not written (only the texture has the copy); cw x ch: the part written */
#define MAX_ALIAS 8
static Alias aliases[MAX_ALIAS]; static unsigned alias_seq;
static int gpucopy_off(void) { static int off = -1; if (off < 0) off = getenv("PWR_VK_NO_GPUCOPY") != NULL; return off; }
static void alias_free(Alias *a) {
    tex_delete(&a->tex); tex_delete(&a->down);
    if (a->pbo) glDeleteBuffers(1, &a->pbo);
    if (a->fence) glDeleteSync(a->fence);
    memset(a, 0, sizeof *a);
}
static void aliases_drop(void) { for (int i = 0; i < MAX_ALIAS; i++) alias_free(&aliases[i]); }
static void aliases_forget(void) { for (int i = 0; i < MAX_ALIAS; i++) aliases[i].deferred = 0; }
static int alias_current(Alias *a) {   /* its guest bytes are unchanged (checked once per command list) */
    if (a->checked_list != list_no) { a->checked_list = list_no; if (hash_mem(a->addr, a->span) != a->hash) { a->valid = 0; return 0; } }
    return 1;
}
/* the copy just made into a: its read-back at 1x, a fence after it (the caller flushes) */
static void alias_issue(Alias *a) {
    const uint32_t S = (uint32_t)cur_scale;
    GLuint from = a->tex;
    if (S > 1) {
        if (!a->down) a->down = tex_new(a->w, a->h, 0, 1);
        blit_color(a->tex, 0, 0, (GLint)(a->w * S), (GLint)(a->h * S), a->down, 0, 0, (GLint)a->w, (GLint)a->h, GL_LINEAR);
        from = a->down;
    }
    read_from(from);
    if (!a->pbo) glGenBuffers(1, &a->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, a->pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)a->w * a->h * 4, NULL, GL_STREAM_READ);
    glReadPixels(0, 0, (GLsizei)a->w, (GLsizei)a->h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (a->fence) glDeleteSync(a->fence);
    a->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    a->deferred = 1;
}
/* an alias's read-back -> its guest bytes (the texture's tiled RGBA8, the part the copy wrote), pica_vk.c's
   alias_writeback: on the GX worker, or the main thread with the worker idle */
static void alias_writeback(Alias *a) {
    if (!a->deferred || !gl_current()) return;
    a->deferred = 0;
    if (a->fence) { glClientWaitSync(a->fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull); glDeleteSync(a->fence); a->fence = NULL; }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, a->pbo);
    const uint8_t *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)a->w * a->h * 4, GL_MAP_READ_BIT);
    if (src) {   /* texture row j = memory row H - 1 - j; the copy wrote memory rows 0..ch-1, columns 0..cw-1 */
        static const uint8_t mx2[8] = { 0, 1, 4, 5, 16, 17, 20, 21 }, my2[8] = { 0, 2, 8, 10, 32, 34, 40, 42 };
        uint8_t *base = g_mem + a->addr;
        for (uint32_t r2 = 0; r2 < a->ch; r2++) {
            const uint8_t *row = src + (size_t)(a->h - 1 - r2) * a->w * 4;
            for (uint32_t x = 0; x < a->cw; x++) {
                uint8_t *q = base + (mx2[x & 7] + my2[r2 & 7] + (x & ~7u) * 8 + (r2 & ~7u) * a->w) * 4;
                const uint8_t *c = row + (size_t)x * 4;
                q[0] = c[3]; q[1] = c[2]; q[2] = c[1]; q[3] = c[0];   /* RGBA8 in memory: A, B, G, R */
            }
        }
    }
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    a->hash = hash_mem(a->addr, a->span); a->checked_list = list_no;
}
static int aliases_touch(uint32_t start, uint32_t end) {
    for (int i = 0; i < MAX_ALIAS; i++) { const Alias *a = &aliases[i]; if (a->deferred && start < a->addr + a->span && a->addr < end) return 1; }
    return 0;
}
static void aliases_writeback(uint32_t start, uint32_t end) {
    for (int i = 0; i < MAX_ALIAS; i++) { Alias *a = &aliases[i]; if (a->deferred && start < a->addr + a->span && a->addr < end) alias_writeback(a); }
}
/* gsp.c: a GX operation overwrites these bytes: copies wholly inside need never land */
void pica_gl_alias_discard(uint32_t start, uint32_t end) {
    if (state != 1) return;
    for (int i = 0; i < MAX_ALIAS; i++) { Alias *a = &aliases[i]; if (a->deferred && a->addr >= start && a->addr + a->span <= end) a->deferred = 0; }
}
static Alias *alias_for(uint32_t out, const Alias *keep) {   /* the alias to make at out: its own, a free one, the oldest */
    Alias *a = NULL;
    for (int i = 0; i < MAX_ALIAS && !a; i++) if (aliases[i].tex && aliases[i].addr == out && &aliases[i] != keep) a = &aliases[i];
    for (int i = 0; i < MAX_ALIAS && !a; i++) if (!aliases[i].tex) a = &aliases[i];
    for (int k = 0; k < MAX_ALIAS && !a; k++) { Alias *c = &aliases[alias_seq++ % MAX_ALIAS]; if (c != keep) a = c; }
    if (a && a->deferred && a->addr != out) alias_writeback(a);   /* another address's bytes, not landed yet */
    return a;
}
static void aliases_overwritten(uint32_t start, uint32_t end, const Alias *keep) {
    for (int i = 0; i < MAX_ALIAS; i++) if (&aliases[i] != keep && aliases[i].valid && aliases[i].addr < end && start < aliases[i].addr + aliases[i].span) aliases[i].valid = 0;
}
static GLuint alias_tex(uint32_t addr, uint32_t w, uint32_t h, int fmt, int levels) {
    if (levels != 1) return 0;
    for (int i = 0; i < MAX_ALIAS; i++) {
        Alias *a = &aliases[i];
        if (!a->valid || a->addr != addr || a->w != w || a->h != h || (int)a->fmt != fmt) continue;
        return alias_current(a) ? a->tex : 0;
    }
    return 0;
}

/* gsp.c's TextureCopy of a render target's whole tile rows (pica_vk_texture_copy's conditions); 1 when done here */
int pica_gl_texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_line, uint32_t out_line) {
    if (!gl_here() || gpucopy_off()) return 0;
    aliases_overwritten(out, out + size, NULL);
    Surf *s = NULL;
    for (int i = 0; i < nsurf; i++) if (!surfs[i].depth && !surfs[i].sub && surfs[i].addr == in && surfs[i].synced && surf_size(&surfs[i]) == size) { s = &surfs[i]; break; }
    if (!s) return 0;
    const uint32_t bpp = surf_bpp(s), iw = (in_line & 0xffff) * 16, ig = (in_line >> 16) * 16, ow = (out_line & 0xffff) * 16, og = (out_line >> 16) * 16;
    if (iw != s->w * 8 * bpp || ig || ow != iw || (iw + og) % (8 * bpp)) return 0;
    const uint32_t W = (iw + og) / (8 * bpp), S = (uint32_t)cur_scale, span = (s->h / 8) * (iw + og);
    uint32_t H = 8; while (H < s->h) H <<= 1;   /* the texture's shape: its width from the stride, a power-of-two height */
    if (batch_open) pica_gl_flush();
    Alias *a = alias_for(out, NULL);
    if (!a->tex || a->w != W || a->h != H) {
        tex_delete(&a->tex); tex_delete(&a->down);
        a->tex = tex_new(W * S, H * S, 0, 1);
        const GLfloat zero[4] = { 0, 0, 0, 0 }; clear_color(a->tex, zero);
    }
    const GLint y = (GLint)((H - s->h) * S);
    blit_color(s->tex, 0, 0, (GLint)(s->w * S), (GLint)(s->h * S), a->tex, 0, y, (GLint)(s->w * S), y + (GLint)(s->h * S), GL_NEAREST);
    a->addr = out; a->w = W; a->h = H; a->fmt = (uint32_t)s->fmt; a->span = span; a->valid = 1; a->cw = s->w; a->ch = s->h;
    alias_issue(a);
    glFlush();
    a->hash = hash_mem(out, span); a->checked_list = list_no;   /* the bytes there now, which the copy's stand for */
    return 1;
}

/* gsp.c's DisplayTransfer halving such a copy, tiled RGBA8 to tiled RGBA8 (pica_vk_transfer_gpu's conditions): a linear
   blit between the aliases; 1 when done here */
int pica_gl_transfer_gpu(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags) {
    if (!gl_here() || gpucopy_off()) return 0;
    const uint32_t iw = in_dim & 0xffff, ih = in_dim >> 16, ow = (out_dim & 0xffff) / 2, oh = (out_dim >> 16) / 2;
    if (((flags >> 24) & 3) != 2 || !((flags >> 5) & 1) || ((flags >> 1) & 1) || (flags & 1) || ((flags >> 8) & 7) || ((flags >> 12) & 7) || ow * 2 != iw || oh * 2 != ih) return 0;
    Alias *src = NULL;
    for (int i = 0; i < MAX_ALIAS; i++) if (aliases[i].valid && aliases[i].addr == in && aliases[i].w == iw && aliases[i].h == ih && aliases[i].fmt == 0) src = &aliases[i];
    if (!src || !alias_current(src)) return 0;
    aliases_overwritten(out, out + ow * oh * 4, src);
    Alias *a = alias_for(out, src);
    if (!a) return 0;
    const uint32_t S = (uint32_t)cur_scale;
    if (!a->tex || a->w != ow || a->h != oh) { tex_delete(&a->tex); tex_delete(&a->down); a->tex = tex_new(ow * S, oh * S, 0, 1); }
    blit_color(src->tex, 0, 0, (GLint)(iw * S), (GLint)(ih * S), a->tex, 0, 0, (GLint)(ow * S), (GLint)(oh * S), GL_LINEAR);
    a->addr = out; a->w = ow; a->h = oh; a->fmt = 0; a->span = ow * oh * 4; a->valid = 1; a->cw = ow; a->ch = oh;
    alias_issue(a);
    glFlush();
    a->hash = hash_mem(out, a->span); a->checked_list = list_no;
    return 1;
}

/* ---------------- scan-out at the internal resolution (pica_vk.c's) ----------------
   Each DisplayTransfer of a display target into an LCD framebuffer copies the target's texture into one kept for that
   framebuffer address, which the presenter shows (video_gl.c hi_screen) as long as nothing else wrote the framebuffer
   since. Between the two contexts: a fence after each copy (the presenter waits for it), and one after the presenter's
   use of them (pica_gl_scan_shown: the next copy waits for it), all under scan_lock. */
#define MAX_SCAN 8
static struct { uint32_t addr, end, w, h, tw, th; int valid; GLuint tex; GLsync done; } scans[MAX_SCAN];
static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
static GLsync scan_shown;
void pica_gl_scan_lock(int on) { if (on) pthread_mutex_lock(&scan_lock); else pthread_mutex_unlock(&scan_lock); }
int pica_gl_scanout(uint32_t addr, GLuint *tex, uint32_t *w, uint32_t *h, GLsync *done) {   /* the presenter, under the lock */
    for (int i = 0; i < MAX_SCAN; i++) if (scans[i].valid && scans[i].addr == addr) { *tex = scans[i].tex; *w = scans[i].tw; *h = scans[i].th; *done = scans[i].done; return 1; }
    return 0;
}
void pica_gl_scan_shown(GLsync shown) {   /* the presenter, under the lock (flushed) */
    if (scan_shown) glDeleteSync(scan_shown);
    scan_shown = shown;
}
void pica_gl_scanout_invalidate(uint32_t start, uint32_t end) {   /* a GX operation wrote guest bytes in [start, end) */
    pthread_mutex_lock(&scan_lock);
    for (int i = 0; i < MAX_SCAN; i++) if (scans[i].valid && start < scans[i].end && scans[i].addr < end) scans[i].valid = 0;
    pthread_mutex_unlock(&scan_lock);
}
/* The bottom screen's pixels for Samus Returns' compositor (video.c classifies them every frame), read back at 1x from its
   scan-out copy without waiting (pica_vk.c's rb_*): the presenter takes the newest finished one (pica_gl_bottom_pixels) */
#define NRB 4
static struct { GLuint pbo; GLsync fence; uint32_t addr; int pending; } rb[NRB]; static int rb_next;
static GLuint rb_tex;
static uint8_t rb_last[2][240 * 320 * 4]; static uint32_t rb_last_addr[2]; static unsigned rb_last_seq[2], rb_seq;
static void rb_harvest(int k, int wait) {   /* under scan_lock: a finished read-back kept by framebuffer address (two) */
    if (!rb[k].pending || !gl_current()) return;
    if (!wait) { const GLenum st = glClientWaitSync(rb[k].fence, 0, 0); if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED) return; }
    else glClientWaitSync(rb[k].fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000000ull);
    glDeleteSync(rb[k].fence); rb[k].fence = NULL; rb[k].pending = 0;
    const int j = rb_last_addr[0] == rb[k].addr ? 0 : rb_last_addr[1] == rb[k].addr ? 1 : rb_last_seq[0] <= rb_last_seq[1] ? 0 : 1;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, rb[k].pbo);
    const uint8_t *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sizeof rb_last[j], GL_MAP_READ_BIT);
    if (src) { memcpy(rb_last[j], src, sizeof rb_last[j]); rb_last_addr[j] = rb[k].addr; rb_last_seq[j] = ++rb_seq; }
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}
/* video.c (main thread): the bottom screen's pixels as decode_screen makes them from guest memory (320 x 240 RGBA, x
   across, y down), from the newest finished read-back of the framebuffer at addr, else of the other one; 0 when none */
int pica_gl_bottom_pixels(uint32_t addr, uint8_t *out) {
    if (state != 1) return 0;
    pthread_mutex_lock(&scan_lock);
    for (int i = 0; i < NRB; i++) rb_harvest(i, 0);
    int k = rb_last_addr[0] == addr && rb_last_seq[0] ? 0 : rb_last_addr[1] == addr && rb_last_seq[1] ? 1 : -1;
    if (k < 0) k = rb_last_seq[0] > rb_last_seq[1] ? 0 : rb_last_seq[1] ? 1 : -1;
    if (k >= 0) {
        const uint8_t *b = rb_last[k];   /* texture row r = memory line 319 - r, column c = pixel c: screen (x, y) = (row 319 - x, column 239 - y) */
        for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) {
            const uint8_t *p = b + ((size_t)(319 - x) * 240 + (size_t)(239 - y)) * 4; uint8_t *d = out + ((size_t)y * 320 + (size_t)x) * 4;
            d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 255;
        }
    }
    pthread_mutex_unlock(&scan_lock);
    return k >= 0;
}
static void scans_drop(void) {
    pthread_mutex_lock(&scan_lock);
    for (int i = 0; i < MAX_SCAN; i++) { tex_delete(&scans[i].tex); if (scans[i].done) glDeleteSync(scans[i].done); memset(&scans[i], 0, sizeof scans[i]); }
    pthread_mutex_unlock(&scan_lock);
}

/* gsp.c's DisplayTransfer: what Samus Returns' features need from it, and the scan-out copy (1: the CPU's is not needed) */
static uint32_t bloom_out[8]; static int nbloom_out;
int pica_gl_display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags) {
    msr_bottom_frame();   /* a frame was finished: the next bottom-screen draw starts one */
    if ((flags >> 24) & 3) { int k = 0; for (; k < nbloom_out && bloom_out[k] != out; k++) {} if (k == nbloom_out && nbloom_out < 8) bloom_out[nbloom_out++] = out; }
    const uint32_t iw = in_dim & 0xffff, ih = in_dim >> 16, ow = out_dim & 0xffff, oh = out_dim >> 16;
    static const uint32_t obpp[8] = { 4, 3, 2, 2, 2, 4, 4, 4 };
    pica_gl_scanout_invalidate(out, out + ow * oh * obpp[(flags >> 12) & 7]);
    if (!gl_here()) return 0;
    /* tiled RGBA8 -> linear RGB8, no flip or scaling: the whole target or its first oh memory lines (the bottom screen's
       320 of 400), from the target drawn last at that address that holds them */
    if (((flags >> 8) & 7) != 0 || ((flags >> 12) & 7) != 1 || (flags & 0x0300000Bu) || iw != ow || !oh || oh > ih) return 0;
    Surf *src = NULL;
    for (int i = 0; i < nsurf; i++) {
        Surf *c = &surfs[i];
        if (c->depth || c->addr != in || c->w != iw || c->fmt != 0 || !c->synced || c->h < oh || c->h > ih) continue;
        if (!src || c->use_seq > src->use_seq) src = c;
    }
    if (!src) return 0;
    if (batch_open) pica_gl_flush();
    const uint32_t S = (uint32_t)cur_scale, W = ow * S, H = oh * S;
    pthread_mutex_lock(&scan_lock);
    int k = -1;
    for (int i = 0; i < MAX_SCAN; i++) if (scans[i].tex && scans[i].addr == out && scans[i].w == ow && scans[i].h == oh) { k = i; break; }
    for (int i = 0; k < 0 && i < MAX_SCAN; i++) if (!scans[i].tex) k = i;
    if (k < 0) { pthread_mutex_unlock(&scan_lock); return 0; }
    if (!scans[k].tex || scans[k].tw != W || scans[k].th != H) {
        tex_delete(&scans[k].tex);
        scans[k].tex = tex_new(W, H, 0, 1);
        scans[k].addr = out; scans[k].w = ow; scans[k].h = oh; scans[k].tw = W; scans[k].th = H; scans[k].end = out + ow * oh * 3;
    }
    if (scan_shown) { glWaitSync(scan_shown, 0, GL_TIMEOUT_IGNORED); glDeleteSync(scan_shown); scan_shown = NULL; }   /* after the presenter's reads */
    const GLint y0 = (GLint)((src->h - oh) * S);   /* memory line y is texture row h - 1 - y: the lines are the last oh rows */
    blit_color(src->tex, 0, y0, (GLint)W, y0 + (GLint)H, scans[k].tex, 0, 0, (GLint)W, (GLint)H, GL_NEAREST);
    if (scans[k].done) glDeleteSync(scans[k].done);
    scans[k].done = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (oh < ih && ow == 240 && oh == 320) {   /* the bottom screen: also filtered to 1x and read back (pica_gl_bottom_pixels) */
        const int j = rb_next; rb_next = (rb_next + 1) % NRB;
        rb_harvest(j, 1);   /* (its last use: frames ago) */
        if (!rb_tex) rb_tex = tex_new(240, 320, 0, 1);
        blit_color(scans[k].tex, 0, 0, (GLint)W, (GLint)H, rb_tex, 0, 0, 240, 320, GL_LINEAR);
        read_from(rb_tex);
        if (!rb[j].pbo) { glGenBuffers(1, &rb[j].pbo); glBindBuffer(GL_PIXEL_PACK_BUFFER, rb[j].pbo); glBufferData(GL_PIXEL_PACK_BUFFER, 240 * 320 * 4, NULL, GL_STREAM_READ); }
        else glBindBuffer(GL_PIXEL_PACK_BUFFER, rb[j].pbo);
        glReadPixels(0, 0, 240, 320, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        rb[j].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0); rb[j].addr = out; rb[j].pending = 1;
    }
    glFlush();   /* (a fence another context waits for must have been flushed) */
    scans[k].valid = 1;
    pthread_mutex_unlock(&scan_lock);
    return 1;   /* the presenter shows the copy: gsp.c skips the CPU's */
}

/* the player's Bloom setting: pica_vk.c bloom_scale */
static void bloom_scale(uint8_t *fsu) {
    float k = msr_bloom();
    if (k >= .999f || !nbloom_out) return;
    if (k < 0) k = 0;
    static const int tb[6] = { 0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8 }, addrr[3] = { 0x085, 0x095, 0x09D };
    const uint32_t te = R(0x080);
    for (int u = 0; u < 3; u++) {
        if (!((te >> u) & 1)) continue;
        const uint32_t a = pa_to_va(R(addrr[u]) << 3); int blurred = 0;
        for (int i = 0; i < nbloom_out; i++) blurred |= bloom_out[i] == a;
        if (!blurred) continue;
        for (int st = 0; st < 5; st++) {
            const uint32_t src = R(tb[st]), s0 = src & 15, s1 = src >> 4 & 15, cmb = R(tb[st] + 2) & 15, next = R(tb[st + 1] + 2) & 15;
            if (cmb != 1 || !((s0 == 3u + (uint32_t)u && s1 == 0xe) || (s0 == 0xe && s1 == 3u + (uint32_t)u)) || (next != 2 && next != 8 && next != 9)) continue;
            uint8_t *c = fsu + (size_t)(tb[st] + 3 - 0x040) * 4;
            for (int ch = 0; ch < 3; ch++) c[ch] = (uint8_t)((float)c[ch] * k + .5f);
        }
    }
}

/* ---------------- draws ---------------- */
#define UBO_BYTES (129 * 16)      /* pica.frag's Draw: registers 0x040-0x1DF, 8 + 8 + 8 light vectors, misc */
#define VS_UBO_BYTES (126 * 16)   /* the vertex shaders' VsDraw: c[96], ib, iu, perm, omap[2], fa[12], fdef[12], xf */
static const GLenum blend_factor[15] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
    GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
    GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_SRC_ALPHA_SATURATE };
static const GLenum blend_eq[5] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX };
static const GLenum compare[8] = { GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL };
static const GLenum stencil_op[8] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
static GLenum bfac(uint32_t f) { return f < 15 ? blend_factor[f] : GL_ONE; }
static GLenum beq(uint32_t e) { return e < 5 ? blend_eq[e] : GL_FUNC_ADD; }

static void apply(const DrawSetup *d) {
    const int all = !st_valid;
    if (all || bound_fbo != d->fbo) { glBindFramebuffer(GL_FRAMEBUFFER, d->fbo); bound_fbo = d->fbo; }
    if (all || last.prog != d->prog) glUseProgram(d->prog);
    if (all || memcmp(last.vp, d->vp, sizeof d->vp)) glViewport(d->vp[0], d->vp[1], d->vp[2], d->vp[3]);
    if (!scissor_on) { glEnable(GL_SCISSOR_TEST); scissor_on = 1; }
    if (all || memcmp(last.sc, d->sc, sizeof d->sc)) glScissor(d->sc[0], d->sc[1], d->sc[2], d->sc[3]);
    if (all || last.blend != d->blend) { if (d->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
    if (all || memcmp(last.sfac, d->sfac, sizeof d->sfac) || memcmp(last.dfac, d->dfac, sizeof d->dfac))
        glBlendFuncSeparate(d->sfac[0], d->dfac[0], d->sfac[1], d->dfac[1]);
    if (all || memcmp(last.eq, d->eq, sizeof d->eq)) glBlendEquationSeparate(d->eq[0], d->eq[1]);
    if (all || memcmp(last.bc, d->bc, sizeof d->bc)) glBlendColor(d->bc[0], d->bc[1], d->bc[2], d->bc[3]);
    if (all || last.cmask != d->cmask) glColorMask(d->cmask & 1, (d->cmask >> 1) & 1, (d->cmask >> 2) & 1, (d->cmask >> 3) & 1);
    if (all || last.dt != d->dt) { if (d->dt) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); }
    if (all || last.dw != d->dw) glDepthMask(d->dw);
    if (all || last.dfn != d->dfn) glDepthFunc(d->dfn);
    if (all || last.ste != d->ste) { if (d->ste) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST); }
    if (all || last.sfn != d->sfn || last.sref != d->sref || last.scmask != d->scmask) glStencilFunc(d->sfn, d->sref, d->scmask);
    if (all || memcmp(last.sop, d->sop, sizeof d->sop)) glStencilOp(d->sop[0], d->sop[1], d->sop[2]);
    if (all || last.swmask != d->swmask) glStencilMask(d->swmask);
    if (all || last.cull != d->cull) {
        if (!d->cull) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glCullFace(d->cull == 1 ? GL_BACK : GL_FRONT); }
    }
    last = *d; st_valid = 1;
}

static void pica_gl_draw_(const Vtx *vcache, const uint32_t *vlist_index, const uint32_t *vlist_slot, uint32_t nvlist,
                          const uint32_t *slots, uint32_t n, int topo);
void pica_gl_draw(const Vtx *vcache, const uint32_t *vlist_index, const uint32_t *vlist_slot, uint32_t nvlist,
                  const uint32_t *slots, uint32_t n, int topo) {
    PROF(5, pica_gl_draw_(vcache, vlist_index, vlist_slot, nvlist, slots, n, topo));
}
/* vcache: the CPU-shaded vertices by cache slot, or NULL to shade on the GPU (pica_vk_draw's meaning) */
static void pica_gl_draw_(const Vtx *vcache, const uint32_t *vlist_index, const uint32_t *vlist_slot, uint32_t nvlist,
                          const uint32_t *slots, uint32_t n, int topo) {
    const int gpu = vcache == NULL;
    if (!gl_here() || (gpu && !gpu_vs)) return;
    /* Samus Returns' single screen: the title menus' backdrop quads on the bottom screen are left out, and the top screen's
       world draws are squeezed to 16:9 and zoomed in clip space (pica_vk.c, the same) */
    float msx = 1, msy = 1;
    if (msr_single()) {
        const uint32_t fw = R(0x11E) & 0x7ff, fh = ((R(0x11E) >> 12) & 0x3ff) + 1;
        if (fw == 240 && fh == 320) {
            const uint32_t dim = R(0x082);
            const int quad = n == 6 && (R(0x080) & 1) && (dim >> 16 & 0x7ff) == 1024 && (dim & 0x7ff) == 512 && (R(0x08E) & 15) == 7;
            if (msr_skip_bottom_draw(quad)) return;
        } else msr_bottom_frame();
        if (f24(R(0x041)) == 120.f && f24(R(0x043)) == 200.f) {   /* the top screen's viewport */
            static float world_px;
            const V4 c3 = pica_vs_cf[3];
            const int camera = c3.x == 0.f && c3.y == 0.f && c3.z == -1.f && c3.w == 0.f, depth = R(0x107) & 1, shadow = (R(0x100) & 3) == 3;
            const float px = -pica_vs_cf[1].x;
            if (depth && !shadow) {
                msr_world_scale(&msx, &msy);
                if (camera) { const float g = msr_fov_gain(px); msx *= g; msy *= g; world_px = px; }
            } else if (depth && camera && world_px > 0.f && fabsf(px - world_px) < 1e-3f * world_px) {
                msx = msy = msr_fov_gain(px);
            }
        }
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        if (ensure_pass()) return;
        GLuint tex[3]; int flush = 0;
        const uint32_t tex_en = R(0x080);
        for (int u = 0; u < 3 && !flush; u++) PROF(2, tex[u] = (tex_en >> u) & 1 ? tex_get(u, &flush) : white);
        if (flush) {   /* a target drawn in this list overlaps a texture: into guest memory first */
            if (attempt) { fprintf(stderr, "pica_gl: a draw's texture overlaps its own target, dropped\n"); return; }
            pica_gl_flush(); continue;
        }
        /* indices: lists, strips (alternate winding as pica_sw.c) and fans -> a triangle list over the distinct vertices */
        static uint32_t remap[65536], tri_idx[3 * 65536];
        uint32_t nidx = 0;
        #define PUT(a, b, c) (tri_idx[nidx++] = (a), tri_idx[nidx++] = (b), tri_idx[nidx++] = (c))
        if (topo == 1) { for (uint32_t i = 2; i < n; i++) { if (i & 1) PUT(i - 1, i - 2, i); else PUT(i - 2, i - 1, i); } }
        else if (topo == 2) { for (uint32_t i = 2; i < n; i++) PUT(0, i - 1, i); }
        else { for (uint32_t i = 0; i + 2 < n; i += 3) PUT(i, i + 1, i + 2); }
        #undef PUT
        if (!nidx) return;
        for (uint32_t i = 0; i < nvlist; i++) remap[vlist_slot[i]] = i;
        /* the program: the vertex shader (the translation once it is ready, else the interpreter; or pica.vert) with the
           fragment shader specialised for this draw, else the generic one */
        const int nd = (R(0x06D) & 1) && (R(0x04D) & 0xffffff) == 0xBF0000 && (R(0x04E) & 0x7fffff) == 0;   /* the usual depth mapping: no gl_FragDepth */
        uint32_t sc[NSC]; uint64_t fspec = 0;
        if (spec_on) { draw_sc(sc); fspec = spec_key(sc, nd); }
        uint32_t vs = VS_CPU;
        if (gpu) { const int gi = gen_current(); vs = gi >= 0 ? VS_GEN + (uint32_t)gi : VS_INTERP; }
        GLuint program = spec_on ? prog_ready(vs, fspec, nd, sc) : 0;
        if (!program && vs >= VS_GEN) program = prog_ready(vs, (uint64_t)nd, nd, NULL);   /* the translation with the generic one */
        if (!program) { vs = gpu ? VS_INTERP : VS_CPU; program = gpu ? prog_interp[nd] : prog_cpu[nd]; }
        const int interp = vs == VS_INTERP;
        /* Attributes for the GPU vertex shaders: the translations read the game's vertex bytes themselves (each loader's used
           range of vertices, copied as it is); the CPU converts them to vec4s for immediate mode, the interpreter, unaligned
           data and sparse index ranges, described to fetch() the same way (f32 x4, stride nattr * 16) */
        static uint8_t vsu[VS_UBO_BYTES];
        struct { uint32_t src, len, roff, used; } ld[12];
        int raw = 0; uint32_t imin = 0, nattr = 0; size_t asize = 0;
        if (gpu) {
            uint32_t *fa = (uint32_t *)(vsu + 101 * 16); float *fdef = (float *)(vsu + 113 * 16);
            nattr = (uint32_t)pica_num_attrs();
            static int noraw = -1; if (noraw < 0) noraw = getenv("PWR_VK_CPU_FETCH") != NULL;
            if (!interp && !noraw) {
                FetchOp op[48]; V4 def[16];
                const int nop = pica_fetch_ops(op, def);
                int ok = nop >= 0 && nattr <= 12;
                uint32_t seen = 0, imax = 0; imin = UINT32_MAX;
                for (uint32_t i = 0; ok && i < nvlist; i++) { if (vlist_index[i] < imin) imin = vlist_index[i]; if (vlist_index[i] > imax) imax = vlist_index[i]; }
                if (ok && nvlist && imax - imin + 1 > 4 * nvlist + 64) ok = 0;   /* sparse: most of the range unused */
                memset(ld, 0, sizeof ld);
                for (int o = 0; ok && o < nop; o++) {
                    const uint32_t sz = op[o].type < 2 ? 1 : op[o].type == 2 ? 2 : 4, al = sz == 4 ? 3 : sz == 2 ? 1 : 0;
                    if ((seen >> op[o].dst & 1) || (op[o].addr & al) || (op[o].stride & al)) { ok = 0; break; }
                    seen |= 1u << op[o].dst;
                    const uint32_t lo = op[o].addr + imin * op[o].stride, hi = op[o].addr + imax * op[o].stride + op[o].n * sz;
                    if (!ld[op[o].loader].used) { ld[op[o].loader].used = 1; ld[op[o].loader].src = lo; ld[op[o].loader].len = hi; }
                    else { if (lo < ld[op[o].loader].src) ld[op[o].loader].src = lo; if (hi > ld[op[o].loader].len) ld[op[o].loader].len = hi; }
                }
                if (ok) {
                    for (int l = 0; l < 12; l++) if (ld[l].used) {
                        ld[l].src &= ~3u; ld[l].len = (ld[l].len - ld[l].src + 3) & ~3u;
                        ld[l].roff = (uint32_t)asize; asize += ld[l].len;
                    }
                    for (uint32_t a = 0; a < 12; a++) { fa[4 * a] = fa[4 * a + 1] = fa[4 * a + 2] = fa[4 * a + 3] = 0; memcpy(fdef + 4 * a, a < nattr ? &def[a] : &(V4){ 0, 0, 0, 1 }, 16); }
                    for (int o = 0; o < nop; o++) {
                        uint32_t *f = fa + 4 * op[o].dst;
                        f[0] = ld[op[o].loader].roff + (op[o].addr + imin * op[o].stride - ld[op[o].loader].src);
                        f[1] = op[o].stride; f[2] = op[o].type | (uint32_t)op[o].n << 4 | 0x100u;
                    }
                    raw = 1;
                }
            }
            if (!raw) {
                for (uint32_t a = 0; a < 12; a++) { fa[4 * a] = a * 16; fa[4 * a + 1] = nattr * 16; fa[4 * a + 2] = 3 | 4 << 4 | 0x100u; fa[4 * a + 3] = 0; memset(fdef + 4 * a, 0, 16); }
                asize = (size_t)nvlist * nattr * 16;
                imin = 0;
            }
            memcpy(vsu, pica_vs_cf, 96 * 16);
            uint32_t *w = (uint32_t *)(vsu + 96 * 16);
            w[0] = R(0x2B0); w[1] = R(0x2BA) & 0xffff; w[2] = R(0x2BD); w[3] = R(0x04F);
            w[4] = R(0x2B1); w[5] = R(0x2B2); w[6] = R(0x2B3); w[7] = R(0x2B4);
            w[8] = R(0x2BB); w[9] = R(0x2BC); w[10] = nattr; w[11] = 0;   /* (w[11]: where the attributes start, below) */
            for (int i = 0; i < 7; i++) w[12 + i] = R(0x050 + i);
            w[19] = 0;
            float *xf = (float *)(vsu + 125 * 16); xf[0] = msx; xf[1] = msy; xf[2] = xf[3] = 0;   /* clip-space scale (above) */
            if (prog_version != pica_vs_version) {
                glBindBuffer(GL_TEXTURE_BUFFER, prog_buf); glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof pica_vs_prog, pica_vs_prog);
                prog_version = pica_vs_version;
            }
        }
        /* the fragment shader's uniforms: registers 0x040-0x1DF and the decoded light parameters (pica_vk.c) */
        static uint8_t fsu[UBO_BYTES], last_fsu[UBO_BYTES]; static GLintptr last_fs_off = -1; static unsigned last_fs_gen;
        memcpy(fsu, &pica_regs[0x040], 104 * 16);
        float *lf = (float *)(fsu + 104 * 16);
        for (int l = 0; l < 8; l++) {
            const int b = 0x140 + 0x10 * l;
            lf[4 * l + 0] = pica_f16(R(b + 4)); lf[4 * l + 1] = pica_f16(R(b + 4) >> 16); lf[4 * l + 2] = pica_f16(R(b + 5)); lf[4 * l + 3] = 0;
            int sx = (int)(R(b + 6) & 0x1fff), sy = (int)((R(b + 6) >> 16) & 0x1fff), sz = (int)(R(b + 7) & 0x1fff);
            if (sx & 0x1000) sx -= 0x2000; if (sy & 0x1000) sy -= 0x2000; if (sz & 0x1000) sz -= 0x2000;
            lf[32 + 4 * l + 0] = (float)sx / 2047.f; lf[32 + 4 * l + 1] = (float)sy / 2047.f; lf[32 + 4 * l + 2] = (float)sz / 2047.f; lf[32 + 4 * l + 3] = 0;
            lf[64 + 4 * l + 0] = pica_f20(R(b + 0xA)); lf[64 + 4 * l + 1] = pica_f20(R(b + 0xB)); lf[64 + 4 * l + 2] = lf[64 + 4 * l + 3] = 0;
        }
        lf[96] = f24(R(0x04D)); lf[97] = f24(R(0x04E)); lf[98] = lf[99] = 0;
        bloom_scale(fsu);
        if (lut_version != light_lut_version) {   /* the LUTs changed (shader: lut[6144 + i] is the fog's) */
            glBindBuffer(GL_TEXTURE_BUFFER, lut_buf);
            glBufferSubData(GL_TEXTURE_BUFFER, 0, sizeof light_lut, light_lut);
            glBufferSubData(GL_TEXTURE_BUFFER, sizeof light_lut, sizeof fog_lut, fog_lut);
            lut_version = light_lut_version;
        }
        /* the draw's data in one upload into dbuf, each part where its use wants it aligned: the fragment shader's uniforms
           (the last draw's when they are the same), the vertex shader's, the attributes (or the CPU-shaded vertices, at a
           multiple of their size: drawn with a base vertex), the indices */
        const size_t vbytes = gpu ? 0 : (size_t)nvlist * sizeof(Vtx);
        int fs_same = last_fs_off >= 0 && last_fs_gen == dgen && !memcmp(fsu, last_fsu, sizeof fsu);
        const GLsizeiptr worst = (GLsizeiptr)(UBO_BYTES + VS_UBO_BYTES + asize + vbytes + (size_t)nidx * 4) + 3 * ubo_align + 256 + (GLsizeiptr)sizeof(Vtx) + 16;
        if (worst > dbuf_most()) { fprintf(stderr, "pica_gl: a draw's data does not fit, dropped\n"); return; }
        { const unsigned g = dgen; dbuf_room(worst); if (dgen != g) fs_same = 0; }
        GLintptr cur = dused, first = -1, off_fs = last_fs_off, off_vs = 0, off_attr = 0, off_vtx = 0;
        if (!fs_same) { off_fs = align_up(cur, ubo_align); cur = off_fs + UBO_BYTES; first = off_fs; }
        if (gpu) {
            off_vs = align_up(cur, ubo_align); if (first < 0) first = off_vs; cur = off_vs + VS_UBO_BYTES;
            off_attr = align_up(cur, 256); cur = off_attr + (GLintptr)(asize ? asize : 16);
        } else { off_vtx = align_up(cur, (GLintptr)sizeof(Vtx)); if (first < 0) first = off_vtx; cur = off_vtx + (GLintptr)vbytes; }
        const GLintptr off_idx = align_up(cur, 4); cur = off_idx + (GLintptr)nidx * 4;
        uint8_t *st = dmap ? dmap + first : scratch((size_t)(cur - first));
        if (!st) return;
        if (!fs_same) { memcpy(st + (off_fs - first), fsu, sizeof fsu); memcpy(last_fsu, fsu, sizeof fsu); last_fs_off = off_fs; last_fs_gen = dgen; }
        uint32_t *idx = (uint32_t *)(st + (off_idx - first));
        if (gpu) {
            ((uint32_t *)(vsu + 96 * 16))[11] = (uint32_t)(interp ? off_attr / 16 : off_attr / 4);   /* where this draw's attributes start: vec4s, or words */
            memcpy(st + (off_vs - first), vsu, sizeof vsu);
            uint8_t *ad = st + (off_attr - first);
            if (raw) { for (int l = 0; l < 12; l++) if (ld[l].used) memcpy(ad + ld[l].roff, g_mem + ld[l].src, ld[l].len); }
            else pica_fetch_attrs_n(vlist_index, nvlist, (V4 *)ad);
            if (raw) for (uint32_t i = 0; i < nidx; i++) idx[i] = vlist_index[remap[slots[tri_idx[i]]]] - imin;   /* the vertex's own index */
            else for (uint32_t i = 0; i < nidx; i++) idx[i] = remap[slots[tri_idx[i]]];
        } else {
            Vtx *vd = (Vtx *)(st + (off_vtx - first));
            for (uint32_t i = 0; i < nvlist; i++) vd[i] = vcache[vlist_slot[i]];
            if (msx != 1 || msy != 1) for (uint32_t i = 0; i < nvlist; i++) { vd[i].pos.x *= msx; vd[i].pos.y *= msy; }
            for (uint32_t i = 0; i < nidx; i++) idx[i] = remap[slots[tri_idx[i]]];
        }
        if (!dmap) PROF(3, {
            void *m = glMapBufferRange(GL_COPY_WRITE_BUFFER, first, cur - first,
                                       0x0002 /* GL_MAP_WRITE_BIT */ | 0x0004 /* GL_MAP_INVALIDATE_RANGE_BIT */ | 0x0020 /* GL_MAP_UNSYNCHRONIZED_BIT */);
            if (m) { memcpy(m, st, (size_t)(cur - first)); glUnmapBuffer(GL_COPY_WRITE_BUFFER); }
            else glBufferSubData(GL_COPY_WRITE_BUFFER, first, cur - first, st);
        });
        dused = cur;
        glBindBufferRange(GL_UNIFORM_BUFFER, 0, dbuf, off_fs, UBO_BYTES);
        if (gpu) glBindBufferRange(GL_UNIFORM_BUFFER, 1, dbuf, off_vs, VS_UBO_BYTES);
        /* fixed-function state (pica_vk.c's pipeline key and dynamic state) */
        const uint32_t om = R(0x107), bl = R(0x101), kc = R(0x103);
        const int depth_on = cur_d && ((R(0x114) & 3) || (R(0x115) & 3)), color_on = (R(0x113) & 15) != 0;
        DrawSetup d; memset(&d, 0, sizeof d);
        d.fbo = fbo_for(cur_c->tex, cur_d ? cur_d->tex : 0); d.prog = program;
        const float vw = f24(R(0x041)), vh = f24(R(0x043)), S = (float)cur_scale;
        int vx = (int)(R(0x068) & 0x3ff), vy = (int)(R(0x068) >> 16 & 0x3ff);
        if (vx & 0x200) vx -= 0x400; if (vy & 0x200) vy -= 0x400;
        d.vp[0] = (GLint)lroundf((float)vx * S); d.vp[1] = (GLint)lroundf((float)vy * S); d.vp[2] = (GLint)lroundf(2 * vw * S); d.vp[3] = (GLint)lroundf(2 * vh * S);
        d.sc[2] = (GLint)(cur_c->w * (uint32_t)cur_scale); d.sc[3] = (GLint)(cur_c->h * (uint32_t)cur_scale);
        if (R(0x065) & 3) {
            int ax = (int)(R(0x066) & 0x3ff), ay = (int)(R(0x066) >> 16 & 0x3ff), bx = (int)(R(0x067) & 0x3ff), by = (int)(R(0x067) >> 16 & 0x3ff);
            if (bx >= (int)cur_c->w) bx = (int)cur_c->w - 1; if (by >= (int)cur_c->h) by = (int)cur_c->h - 1;
            const int K = cur_scale;
            d.sc[0] = ax * K; d.sc[1] = ay * K; d.sc[2] = bx >= ax ? (bx - ax + 1) * K : 0; d.sc[3] = by >= ay ? (by - ay + 1) * K : 0;
        }
        d.blend = (uint8_t)((R(0x100) >> 8) & 1);
        d.sfac[0] = bfac(bl >> 16 & 15); d.dfac[0] = bfac(bl >> 20 & 15); d.eq[0] = beq(bl & 7);
        d.sfac[1] = bfac(bl >> 24 & 15); d.dfac[1] = bfac(bl >> 28 & 15); d.eq[1] = beq(bl >> 8 & 7);
        d.bc[0] = (kc & 255) / 255.f; d.bc[1] = (kc >> 8 & 255) / 255.f; d.bc[2] = (kc >> 16 & 255) / 255.f; d.bc[3] = (kc >> 24) / 255.f;
        d.cmask = (uint8_t)(color_on ? (om >> 8) & 15 : 0);
        /* PICA writes depth with the test off (the game's depth-clearing quads); OpenGL only with it on: ALWAYS then */
        d.dt = (uint8_t)(depth_on && (om & 1)); d.dw = (uint8_t)(depth_on && ((om >> 12) & 1)); d.dfn = compare[om >> 4 & 7];
        if (d.dw && !d.dt) { d.dt = 1; d.dfn = GL_ALWAYS; }
        const uint32_t sf = R(0x105), so = R(0x106);
        d.ste = (uint8_t)(depth_on && (sf & 1) && cur_d->fmt == 3);
        d.sfn = compare[sf >> 4 & 7]; d.sref = (GLint)(sf >> 16 & 255); d.scmask = sf >> 24; d.swmask = (R(0x115) & 3) ? (sf >> 8 & 255) : 0;
        d.sop[0] = stencil_op[so & 7]; d.sop[1] = stencil_op[so >> 4 & 7]; d.sop[2] = stencil_op[so >> 8 & 7];
        const int cm = (int)(R(0x040) & 3);   /* pica_sw.c's cull modes, Vulkan's sense (glFrontFace at the start) */
        d.cull = (uint8_t)(cm == 1 ? 1 : cm == 2 ? 2 : 0);
        PROF(4, apply(&d);
             for (int u = 0; u < 3; u++) {
                 if (unit_tex[u] != tex[u]) { activate(u); glBindTexture(GL_TEXTURE_2D, tex[u]); unit_tex[u] = tex[u]; }
                 const GLuint smp = sampler_for(u);
                 if (unit_smp[u] != smp) { glBindSampler((GLuint)u, smp); unit_smp[u] = smp; }
             }
             if (gpu) { bind_vao(vao_gpu); glDrawElements(GL_TRIANGLES, (GLsizei)nidx, GL_UNSIGNED_INT, (const void *)off_idx); }
             else { bind_vao(vao); glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)nidx, GL_UNSIGNED_INT, (const void *)off_idx, (GLint)(off_vtx / (GLintptr)sizeof(Vtx))); });
        cur_c->dirty_batch = batch_no;   /* (depth targets are not marked, as in pica_vk.c: their write-back stays unused) */
        g_vk_draws++; g_vk_verts += nvlist; g_vk_idx += nidx;
        return;
    }
}
