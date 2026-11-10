/* The settings overlay of the emulator-based build on msr-native (overlay_host.cpp): src/msr/overlay.cpp's Dear ImGui
 * menu, drawn by video.c in its presentation pass. Include after SDL.h and vulkan.h. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* After the settings are loaded: the display and sound settings msr-native started with (resolution 0..10, volume %,
   anisotropic filtering 1..16, FSR or linear upscaling, power-saving waits) */
void msr_overlay_start(int fullscreen, int resolution, int vsync, int volume, int aniso, int fsr, int power_saving);
/* Once the presenter exists (video.c holds its queue lock): Vulkan and the colour format it draws into */
void msr_overlay_vk_init(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t family, VkQueue queue,
                         VkFormat format, uint32_t images, VkRenderPass render_pass);   /* render_pass: the Vulkan 1.1 mode's (vk_rp.h), else VK_NULL_HANDLE */
/* Or OpenGL's presenter (video_gl.c), its context current: the same menu through Dear ImGui's OpenGL 3 backend, drawn
   on the window's framebuffer (bound) */
void msr_overlay_gl_init(void);
void msr_overlay_gl_draw(uint32_t width, uint32_t height);
void msr_overlay_shutdown(void);
/* Each presented frame: msr_overlay_frame before drawing (actions, frame count), msr_overlay_panel with the bottom
   screen's panel (x, y, w, h) or NULL, then msr_overlay_draw inside the presentation's dynamic rendering */
void msr_overlay_frame(void);
void msr_overlay_panel(const float *panel);
void msr_overlay_draw(VkCommandBuffer command, uint32_t width, uint32_t height);
/* Input: 1 when the overlay took the event (sx, sy: drawable pixels per window coordinate) */
int  msr_overlay_event(const SDL_Event *e, float sx, float sy);
int  msr_overlay_visible(void);
int  msr_overlay_capturing(void);     /* waiting for a key or pad button to bind */
void msr_overlay_toggle(void);        /* F1, Select */
int  msr_overlay_launcher_mode(void); /* the menu is the launcher's (no game behind it) */
void msr_overlay_fullscreen(int on);  /* fullscreen changed by a hotkey */
void msr_overlay_hdr(int option, float peak, float paper);   /* video.c: HDR possible (2), off on the display (1), absent (0); nits */

#ifdef __cplusplus
}
#endif
