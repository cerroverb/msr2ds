/* OpenGL PICA200 renderer (pica_gl.c): pica_vk.c's public functions forward here while it is on (OpenGL presentation,
 * video_gl.c). Same meanings as their pica_vk_ twins (gpu.h, pica_int.h). */
#ifndef PWR_PICA_GL_H
#define PWR_PICA_GL_H
#include "pica_int.h"

int  pica_gl_init(void);   /* on the GX worker, at the first draw: 0 when the renderer is on */
int  pica_gl_on(void);
void pica_gl_quality(int resolution, int aniso);
void pica_gl_draw(const Vtx *vcache, const uint32_t *vlist_index, const uint32_t *vlist_slot, uint32_t nvlist,
                  const uint32_t *slots, uint32_t n, int topo);
void pica_gl_flush(void);
void pica_gl_filled(uint32_t start, uint32_t end);
void pica_gl_gx_reads(uint32_t start, uint32_t end);
int  pica_gl_display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags);
void pica_gl_state_reset(void);
int  pica_gl_programs(void);
int  pica_gl_gpu_vs(void);
int  pica_gl_texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_line, uint32_t out_line);
int  pica_gl_transfer_gpu(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags);
void pica_gl_scanout_invalidate(uint32_t start, uint32_t end);
void pica_gl_sync_range(uint32_t start, uint32_t end);
void pica_gl_sync_all(void);
void pica_gl_sync_ready(void);
void pica_gl_discard_range(uint32_t start, uint32_t end);
void pica_gl_alias_discard(uint32_t start, uint32_t end);
int  pica_gl_bottom_pixels(uint32_t addr, uint8_t *out);   /* video.c, main thread */
void pica_gl_preload(int on);   /* main.c, before the start: pwr_gl_programs.bin read and written (on) or not */
void pica_gl_saves_wait(void);   /* main.c at exit: the program cache saved if the compiles made something since */
#endif
