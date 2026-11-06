/* GPU side of the runtime: gsp::Gpu service state, GX commands, PICA command lists, presentation. */
#ifndef PWR_GPU_H
#define PWR_GPU_H
#include "kernel.h"

/* physical <-> guest virtual (old-3DS application layout) */
static inline uint32_t pa_to_va(uint32_t pa) {
    if (pa >= 0x18000000u && pa < 0x18600000u) return pa - 0x18000000u + VA_VRAM;     /* VRAM */
    if (pa >= 0x20000000u && pa < 0x28000000u) return pa - 0x20000000u + VA_LINEAR;   /* FCRAM, linear heap */
    return 0;
}

enum { GSP_IRQ_PSC0, GSP_IRQ_PSC1, GSP_IRQ_VBLANK_TOP, GSP_IRQ_VBLANK_BOTTOM, GSP_IRQ_PPF, GSP_IRQ_P3D, GSP_IRQ_DMA };

/* one screen as the LCD scans it out; filled by gsp_vblank() */
typedef struct { uint32_t addr, stride, format; int valid; } ScreenSource;

void gsp_hang_report(void);                                     /* GX/interrupt queue state (stderr), with k_hang_report */
void gsp_vblank(void);                                         /* host 60 Hz tick: framebuffer swaps + VBlank irqs */
int  gsp_screens(ScreenSource *top, ScreenSource *bottom);     /* current scan-out sources; 0 before any setup */
void gsp_handle(KThread *t, uint32_t *cmd, KObject *s);

void pica_run(uint32_t va, uint32_t size);                     /* execute one PICA200 command list */
void pica_run_buf(const uint8_t *buf, uint32_t va, uint32_t size);   /* same, from a copy of the list */
void pica_stats(uint32_t *lists, uint32_t *draws);

/* video.c */
/* display options (main.c command line; also changed at run time by hotkeys / the settings overlay):
   layout: stacked, side, large, top, bottom, auto    fullscreen: off, borderless, exclusive    filter: nearest, linear, sharp, fsr, fxaa
   aspect: keep, integer, stretch                      auto_gameplay: the layout auto mode uses while flying (large or top)
   fsr_sharpness: 0 (most) .. 2 stops    resolution: 3D internal resolution 1..4    aniso: 1, 2, 4, 8, 16    show_fps: 0/1
   select_layout: 0/1 (pad Back/Select cycles layouts)    free_look: 0/1 (right stick orbits the chase camera, freelook.c) */
int  video_option(const char *name, const char *value);   /* 0 ok, -1 unknown name or value */
void pica_vk_quality(int resolution, int aniso);          /* pica_vk.c: internal resolution, anisotropic filtering */
int  pica_vk_display_transfer(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags);   /* gsp.c: keep the
                                                           internal-resolution image of a display target for video.c */
int  pica_vk_texture_copy(uint32_t in, uint32_t out, uint32_t size, uint32_t in_line, uint32_t out_line);   /* 1: done on the GPU */
int  pica_vk_transfer_gpu(uint32_t in, uint32_t out, uint32_t in_dim, uint32_t out_dim, uint32_t flags);     /* 1: done on the GPU */
void pica_vk_sync_ready(void);
int pica_vk_bottom_pixels(uint32_t addr, uint8_t *out);
void pica_vk_alias_discard(uint32_t start, uint32_t end);   /* gsp.c, after a TextureCopy */
void pica_vk_sync_range(uint32_t start, uint32_t end);        /* gsp.c: render targets' bytes there into guest memory first */
void pica_vk_discard_range(uint32_t start, uint32_t end);     /* gsp.c: a MemoryFill overwrites them: targets wholly inside need not land */
void pica_vk_sync_all(void);
void pica_vk_gx_reads(uint32_t start, uint32_t end);           /* gsp.c: a GX operation reads these guest bytes */
void pica_vk_scanout_invalidate(uint32_t start, uint32_t end);   /* gsp.c: a GX operation wrote these guest bytes */
int  video_init(int scale);
void video_set_fps(int fps);
/* the settings overlay's display settings (msr/overlay_host.cpp): also given as --fullscreen, --resolution, --vsync */
void video_set_fullscreen(int on);
void video_set_vsync(int on);
void video_set_resolution(int factor);   /* 1..4, 0 = by the window's height */
void video_set_aniso(int aniso);         /* anisotropic filtering: 1 (off), 2, 4, 8, 16 */
int  video_fullscreen(void);
int  video_resolution(void);
int  video_vsync(void);
int  video_aniso(void);
int  video_filter_fsr(void);             /* the upscaling filter is FSR 1 (video_option "filter") */
void dsp_set_volume(float volume);       /* dsp.c: 0..1 */
int  k_power_saving(void);               /* kernel.c: power-saving waits (sleep, don't spin) */
int  k_cpu_threads(void);                /* kernel.c: the host's processor threads */
void k_background_thread(void);          /* kernel.c: the calling thread below the game's (work no frame waits for) */
void msr_overlay_start(int fullscreen, int resolution, int vsync, int volume, int aniso, int fsr,
                       int power_saving);   /* msr/overlay_host.cpp */   /* before video_init: above 60, frames are presented without waiting for the display (mailbox) */
int  video_frame(void);                                        /* present current screens; 0 when the window closed */
void video_shutdown(void);
#endif
