/* OpenGL presentation: video.c's other backend (--renderer=opengl; with --renderer=auto, where there is no Vulkan 1.3).
 * An OpenGL 3.3 core context on the SDL window. It draws what the Vulkan presenter draws, with the same shaders
 * (shaders/present, msr, fsr_easu, fsr_rcas: their Vulkan-only lines are under #ifdef VULKAN, and CMake embeds their
 * GLSL; gl_load.c compiles it as GLSL 3.30): the screens, uploaded each frame, are drawn by the layout and the filter
 * into `target`, a window-sized texture, then Samus Returns' compositor quads (single screen) and the layout label. The
 * target is in Vulkan's orientation (row 0 at the top), as are the shaders' gl_FragCoord (FSR) and texture rows, and is
 * copied into the window turned over (OpenGL's row 0 is at the bottom); the settings overlay (Dear ImGui's OpenGL 3
 * backend) is drawn on the window last. FSR needs GLSL 4.00 (textureGather); below, it is sharp bilinear. No HDR.
 * Two more contexts share this one's objects: the PICA renderer's (pica_gl.c), current on the GX worker, and one for
 * its vertex shaders' background compiles. The screens come at the internal resolution from the renderer's scan-out
 * textures when it has them (as video.c's hi_screen), each after its fence; a fence after their use goes back to it. */
#include "gl_load.h"
#include "video_gl.h"
#include <vulkan/vulkan.h>
#include "msr/overlay_host.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240

static const char present_vert[] =
#include "present.vert.glsl.inc"
;
static const char present_frag[] =
#include "present.frag.glsl.inc"
;
static const char msr_vert[] =
#include "msr.vert.glsl.inc"
;
static const char msr_frag[] =
#include "msr.frag.glsl.inc"
;
static const char easu_frag[] =
#include "fsr_easu.frag.glsl.inc"
;
static const char rcas_frag[] =
#include "fsr_rcas.frag.glsl.inc"
;

typedef struct { GLuint id; int w, h; } Tex;
typedef struct { float dst[4], src[4]; int mode[4]; float extra[4]; } Push;                  /* present.vert's P */
typedef struct { float dst[4], src[4], style[4]; int flags[4]; float color[4]; } MsrPush;   /* msr.vert's P */

static SDL_Window *win;
static SDL_GLContext ctx, pica_ctx, compile_ctx;   /* the presenter's (this thread's), the PICA renderer's (the GX worker's), its compiles' */
static int pica_bound, compile_bound, overlay_up;
/* pica_gl.c: its scan-out textures, under its lock */
void pica_gl_scan_lock(int on);
int pica_gl_scanout(uint32_t addr, GLuint *tex, uint32_t *w, uint32_t *h, GLsync *done);
void pica_gl_scan_shown(GLsync shown);
static GLuint vao, ubo, fbo_draw, fbo_read;
static GLuint prog_present, prog_msr, prog_easu, prog_rcas;
static Tex tex_top, tex_bot, tex_toast, target, fsr_tmp[2], beam_copy, hi_scr[2];
static int beam_copy_ok;
static GLint ubo_align = 256; static GLintptr ubo_off;
#define UBO_SIZE (64 * 1024)

/* a texture of w x h RGBA8 (made again when the size changes), linear, clamped */
static int tex_size(Tex *t, int w, int h) {
    if (t->id && t->w == w && t->h == h) return 0;
    if (!t->id) glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    t->w = w; t->h = h;
    return 1;
}
static void tex_upload(const Tex *t, const uint8_t *px) {
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

/* draws go into this texture from here (fbo_draw) */
static void draw_into(const Tex *t) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
    glViewport(0, 0, t->w, t->h);
}

/* the next draw's uniform block: a slice of the frame's buffer (orphaned when it fills, and at each frame) */
static void push(const void *data, GLsizeiptr size) {
    if (ubo_off + size > UBO_SIZE) { glBufferData(GL_UNIFORM_BUFFER, UBO_SIZE, NULL, GL_STREAM_DRAW); ubo_off = 0; }
    glBufferSubData(GL_UNIFORM_BUFFER, ubo_off, size, data);
    glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, ubo_off, size);
    ubo_off += (size + ubo_align - 1) / ubo_align * ubo_align;
}

/* a texture into a rectangle of the current target (W x H), as video.c's gpu_quad_view (rot 1: a framebuffer-oriented image) */
static void quad_rot(GLuint prog, const Tex *t, VidRect r, float W, float H, int mode, int alpha, const float extra[4], int rot) {
    if (!r.on || r.w < 1 || r.h < 1 || !prog || !t->id) return;
    Push p = { { r.x / W * 2 - 1, r.y / H * 2 - 1, (r.x + r.w) / W * 2 - 1, (r.y + r.h) / H * 2 - 1 },
               { (float)t->w, (float)t->h, r.w / (float)t->w, r.h / (float)t->h }, { mode, alpha, rot, 0 }, { 0, 0, 0, 0 } };
    if (extra) memcpy(p.extra, extra, sizeof p.extra);
    glUseProgram(prog);
    glBindTexture(GL_TEXTURE_2D, t->id);
    push(&p, sizeof p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void quad(GLuint prog, const Tex *t, VidRect r, float W, float H, int mode, int alpha, const float extra[4]) {
    quad_rot(prog, t, r, W, H, mode, alpha, extra, 0);
}
/* a screen at the internal resolution: the renderer's scan-out texture for the framebuffer at addr (stored as the
   framebuffer is: 240 lines of the screen's width), turned upright into hi_scr[i] (video.c hi_screen). Under the scan-out
   lock. */
static const Tex *hi_screen(int i, uint32_t addr, int screen_w) {
    GLuint t; uint32_t lw, lh; GLsync done;
    if (!addr || !pica_gl_scanout(addr, &t, &lw, &lh, &done) || lw * (uint32_t)screen_w != lh * SCR_H) return NULL;
    if (done) glWaitSync(done, 0, GL_TIMEOUT_IGNORED);   /* the renderer's copy into it first */
    tex_size(&hi_scr[i], (int)lh, (int)lw);
    draw_into(&hi_scr[i]);
    const Tex src = { t, (int)lw, (int)lh };
    quad_rot(prog_present, &src, (VidRect){ 0, 0, (float)lh, (float)lw, 1 }, (float)lh, (float)lw, 0, 0, NULL, 1);
    return &hi_scr[i];
}
/* Samus Returns' compositor: one quad of msr_compose's list, sampling the bottom screen (video.c msr_quad) */
static void msr_quad(const MsrQuad *q, const Tex *bottom, float W, float H) {
    if (q->dst[2] <= 0 || q->dst[3] <= 0) return;
    MsrPush p = { { q->dst[0] / W * 2 - 1, q->dst[1] / H * 2 - 1, (q->dst[0] + q->dst[2]) / W * 2 - 1, (q->dst[1] + q->dst[3]) / H * 2 - 1 },
                  { q->src[0], q->src[1], q->src[2], q->src[3] }, { q->style[0], q->style[1], q->style[2], q->style[3] },
                  { q->feather, q->circle, 0, 0 }, { q->color[0], q->color[1], q->color[2], q->color[3] } };
    glBindTexture(GL_TEXTURE_2D, bottom->id);
    push(&p, sizeof p);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* FSR: EASU upscale of a screen into fsr_tmp[i] at the output size (integer rectangle), before the target's pass */
static void fsr_upscale(int i, const Tex *src, VidRect *r) {
    if (!r->on) return;
    r->x = floorf(r->x + 0.5f); r->y = floorf(r->y + 0.5f); r->w = floorf(r->w + 0.5f); r->h = floorf(r->h + 0.5f);
    if (r->w < 1 || r->h < 1) { r->on = 0; return; }
    tex_size(&fsr_tmp[i], (int)r->w, (int)r->h);
    draw_into(&fsr_tmp[i]);
    float ex[4] = { r->w, r->h, 0, 0 };
    quad(prog_easu, src, (VidRect){ 0, 0, r->w, r->h, 1 }, r->w, r->h, 0, 0, ex);
}

/* Samus Returns, R held (missiles): the HUD's beam slots from the bottom screen as it was before (video.c beam_slots):
   each frame R is not held, the slots' column (x 250-320) is copied into beam_copy */
static const Tex *beam_slots(const Tex *sb) {
    if (msr_beams_held()) return beam_copy_ok && beam_copy.w == sb->w && beam_copy.h == sb->h ? &beam_copy : sb;
    if (tex_size(&beam_copy, sb->w, sb->h)) beam_copy_ok = 0;
    const int x0 = sb->w * 250 / BOT_W;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sb->id, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_draw);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, beam_copy.id, 0);
    glBlitFramebuffer(x0, 0, sb->w, sb->h, x0, 0, sb->w, sb->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    beam_copy_ok = 1;
    return sb;
}

static void APIENTRY gl_debug(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *user) {
    (void)source; (void)id; (void)length; (void)user;
    if (severity != 0x826B /* GL_DEBUG_SEVERITY_NOTIFICATION */) fprintf(stderr, "video: OpenGL says (type %#x): %s\n", type, message);
}

static GLuint program(const char *name, const char *header, const char *vs, const char *fs) {
    GLuint p = msr_gl_program(name, header, vs, fs);
    if (!p) return 0;
    GLuint block = glGetUniformBlockIndex(p, "P");
    if (block != GL_INVALID_INDEX) glUniformBlockBinding(p, block, 0);
    glUseProgram(p);
    GLint loc = glGetUniformLocation(p, "tex"); if (loc >= 0) glUniform1i(loc, 0);
    return p;
}

int vgl_init(SDL_Window *window, char *err, size_t err_size) {
    win = window;
    /* the newest core profile the driver has (some give exactly the version asked for: FSR needs 4.0), down to 3.3 */
    static const int versions[][2] = { { 4, 6 }, { 4, 5 }, { 4, 3 }, { 4, 2 }, { 4, 1 }, { 4, 0 }, { 3, 3 } };
    for (size_t i = 0; i < sizeof versions / sizeof *versions && !ctx; i++) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, versions[i][0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, versions[i][1]);
        ctx = SDL_GL_CreateContext(win);
    }
    if (!ctx) { snprintf(err, err_size, "OpenGL 3.3: %s", SDL_GetError()); return -1; }
    /* the PICA renderer's context (pica_gl.c), sharing this one's textures and buffers: current on the GX worker */
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    pica_ctx = SDL_GL_CreateContext(win);
    if (!pica_ctx) fprintf(stderr, "video: no second OpenGL context for the 3D (%s): it is drawn on the CPU\n", SDL_GetError());
    compile_ctx = pica_ctx ? SDL_GL_CreateContext(win) : NULL;   /* its vertex shaders' compiles, on a thread of their own */
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(win, ctx);
    const char *missing = NULL;
    if (!msr_gl_load(&missing)) { snprintf(err, err_size, "OpenGL lacks %s", missing); return -1; }
    fprintf(stderr, "video: OpenGL %s (%s, %s), GLSL %s\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    if (msr_gl_version() < 33) { snprintf(err, err_size, "OpenGL %d.%d (3.3 is needed)", msr_gl_version() / 10, msr_gl_version() % 10); return -1; }
    if (getenv("PWR_GL_DEBUG")) {   /* the driver's errors and warnings in the log (a debug context: video.c) */
        typedef void (APIENTRY *Callback)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
        void (APIENTRY *set)(Callback, const void *) = (void (APIENTRY *)(Callback, const void *))SDL_GL_GetProcAddress("glDebugMessageCallback");
        if (set) { glEnable(0x92E0 /* GL_DEBUG_OUTPUT */); glEnable(0x8242 /* GL_DEBUG_OUTPUT_SYNCHRONOUS */); set(gl_debug, NULL); }
    }
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);   /* the quads have no vertex attributes: gl_VertexID */
    glGenFramebuffers(1, &fbo_draw); glGenFramebuffers(1, &fbo_read);
    glGenBuffers(1, &ubo); glBindBuffer(GL_UNIFORM_BUFFER, ubo); glBufferData(GL_UNIFORM_BUFFER, UBO_SIZE, NULL, GL_STREAM_DRAW);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &ubo_align); if (ubo_align < 16) ubo_align = 16;
    const char *h330 = "#version 330 core";
    prog_present = program("present", h330, present_vert, present_frag);
    prog_msr = program("msr", h330, msr_vert, msr_frag);
    if (!prog_present || !prog_msr) { snprintf(err, err_size, "OpenGL shaders failed (see the log)"); return -1; }
    if (msr_gl_version() >= 40) {   /* FSR: textureGather, bitfieldExtract (4.00); packHalf2x16 (4.20) only in its unused half paths */
        const char *h400 = msr_gl_version() >= 42 || msr_gl_has("GL_ARB_shading_language_packing")
            ? "#version 400 core\n#extension GL_ARB_shading_language_packing : enable"
            : "#version 400 core\n#define packHalf2x16(v) 0u\n#define unpackHalf2x16(v) vec2(0.0)";
        prog_easu = program("fsr_easu", h400, present_vert, easu_frag);
        prog_rcas = program("fsr_rcas", h400, present_vert, rcas_frag);
    }
    if (!prog_easu || !prog_rcas) fprintf(stderr, "video: no FSR on this OpenGL: sharp bilinear instead\n");
    tex_size(&tex_top, TOP_W, SCR_H); tex_size(&tex_bot, BOT_W, SCR_H);
    GLenum e = glGetError();
    if (e) { snprintf(err, err_size, "OpenGL error %#x setting up", e); return -1; }
    msr_overlay_gl_init(); overlay_up = 1;
    return 0;
}

/* pica_gl.c, on the GX worker: the renderer's context current there. 0 ok. */
int vgl_renderer_bind(void) { if (!pica_ctx || SDL_GL_MakeCurrent(win, pica_ctx)) return -1; pica_bound = 1; return 0; }
/* pica_gl.c's compile thread: the compiles' context current there. 0 ok. */
int vgl_compiler_bind(void) { if (!compile_ctx || SDL_GL_MakeCurrent(win, compile_ctx)) return -1; compile_bound = 1; return 0; }

void vgl_swap_interval(int interval) {
    if (ctx && SDL_GL_SetSwapInterval(interval) && interval) fprintf(stderr, "video: OpenGL swap interval %d: %s\n", interval, SDL_GetError());
}

void vgl_frame(const VglFrame *f) {
    const float W = (float)f->w, H = (float)f->h;
    if (f->w < 1 || f->h < 1) return;
    glBindVertexArray(vao);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, UBO_SIZE, NULL, GL_STREAM_DRAW); ubo_off = 0;   /* last frame's slices stay its own */
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    tex_upload(&tex_top, f->top); tex_upload(&tex_bot, f->bottom);
    if (f->toast) { tex_size(&tex_toast, f->toast_w, f->toast_h); tex_upload(&tex_toast, f->toast); }
    const Tex *st = &tex_top, *sb = &tex_bot;
    glDisable(GL_BLEND);
    {   /* the screens at the internal resolution where the renderer made them; its next copies wait for this frame's use */
        pica_gl_scan_lock(1);
        const Tex *ht = hi_screen(0, f->scan_top, TOP_W), *hb = hi_screen(1, f->scan_bottom, BOT_W);
        if (ht || hb) { pica_gl_scan_shown(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)); glFlush(); }
        pica_gl_scan_lock(0);
        if (ht) st = ht;
        if (hb) sb = hb;
    }
    const Tex *sbeams = f->mf && f->mf->count ? beam_slots(sb) : sb;   /* R held: the beam slots before it */
    VidRect rt = f->rt, rb = f->rb;
    /* the screens' passes: alpha comes from the source (the screens write 1), colour blends by it, as video.c's pipelines */
    glDisable(GL_BLEND);
    const int fsr = f->filter == 3 && prog_easu && prog_rcas;
    if (fsr) { fsr_upscale(0, st, &rt); fsr_upscale(1, sb, &rb); }
    tex_size(&target, f->w, f->h);
    draw_into(&target);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND); glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    if (fsr) {
        float et[4] = { f->sharpness, 0, rt.x, rt.y }, eb[4] = { f->sharpness, 0, rb.x, rb.y };
        quad(prog_rcas, &fsr_tmp[0], rt, W, H, 0, 0, et);
        quad(prog_rcas, &fsr_tmp[1], rb, W, H, 0, 0, eb);
    } else {
        const int m = f->filter == 0 ? 0 : f->filter == 2 || f->filter == 3 ? 2 : f->filter == 4 ? 4 : 1;
        quad(prog_present, st, rt, W, H, m, 0, NULL); quad(prog_present, sb, rb, W, H, m, 0, NULL);
    }
    if (f->mf && f->mf->count) {
        glUseProgram(prog_msr);
        for (int i = 0; i < f->mf->count; i++) msr_quad(&f->mf->quads[i], f->mf->quads[i].source ? sbeams : sb, W, H);
    }
    if (f->toast_show && tex_toast.id)   /* the layout label, top centre (pixel for pixel) */
        quad(prog_present, &tex_toast, (VidRect){ floorf((W - (float)tex_toast.w) / 2), 12, (float)tex_toast.w, (float)tex_toast.h, 1 }, W, H, 0, 1, NULL);
    /* into the window, turned over; then the settings, the FPS counter and the hints, on the window */
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_draw);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, f->w, f->h, 0, f->h, f->w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, f->w, f->h);
    msr_overlay_gl_draw((uint32_t)f->w, (uint32_t)f->h);
    SDL_GL_SwapWindow(win);
}

void vgl_shutdown(void) {
    if (!ctx) return;
    if (overlay_up) msr_overlay_shutdown();
    overlay_up = 0;
    if (pica_ctx && !pica_bound) SDL_GL_DeleteContext(pica_ctx);   /* (once bound, it stays: the GX worker may still use it) */
    if (compile_ctx && !compile_bound) SDL_GL_DeleteContext(compile_ctx);
    pica_ctx = compile_ctx = NULL;
    SDL_GL_DeleteContext(ctx); ctx = NULL;
}
