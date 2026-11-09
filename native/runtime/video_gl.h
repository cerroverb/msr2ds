/* OpenGL presentation (video_gl.c): video.c's other backend, for graphics without Vulkan 1.3. */
#ifndef PWR_VIDEO_GL_H
#define PWR_VIDEO_GL_H
#include <SDL.h>
#include <stddef.h>
#include <stdint.h>
#include "msr/msr.h"

typedef struct { float x, y, w, h; int on; } VidRect;   /* drawable pixels, from the top left */

/* one presented frame, as video.c lays it out */
typedef struct {
    int w, h;                         /* the drawable */
    const uint8_t *top, *bottom;      /* the screens as decoded: 400 x 240 and 320 x 240 RGBA rows */
    VidRect rt, rb;                   /* where they go (rb.on 0: not drawn) */
    int filter;                       /* 0 nearest, 1 linear, 2 sharp bilinear, 3 FSR 1, 4 FXAA */
    float sharpness;                  /* FSR's RCAS, in stops (0 = most) */
    const MsrFrame *mf;               /* the single screen's HUD pieces, menus and prompts over the view (count 0: none) */
    const uint8_t *toast;             /* the layout label's pixels when they changed (toast_w x toast_h RGBA), else NULL */
    int toast_w, toast_h, toast_show; /* its size, and whether it shows (top centre, pixel for pixel) */
    uint32_t scan_top, scan_bottom;   /* the screens' framebuffer addresses (RGB8; 0: none): the renderer's internal-
                                         resolution copies of them stand in for top and bottom (pica_gl.c) */
} VglFrame;

/* the window was made with SDL_WINDOW_OPENGL: its context, functions and the presenter's resources. 0 ok; else -1,
   with why in err. */
int  vgl_init(SDL_Window *window, char *err, size_t err_size);
void vgl_swap_interval(int interval);   /* 1: wait for the display, 0: not */
void vgl_frame(const VglFrame *f);      /* draw, the settings overlay last, and present */
void vgl_shutdown(void);
#endif
