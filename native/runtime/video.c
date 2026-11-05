/* Presentation: SDL2 window + Vulkan swapchain.
 * Each frame the two LCD framebuffers (stored rotated, 240 pixels per line) are decoded into a 400x240 (top) and a
 * 320x240 (bottom) RGBA image. The GPU presenter uploads them as textures and draws them into an off-screen image the
 * size of the window, placed by the screen layout and filtered (nearest, linear, sharp bilinear; FSR/FXAA, see below),
 * then copies that image into the swapchain image. A copy, not a blit or a render into the swapchain: software/linear
 * swapchains (MESA_VK_WSI_DEBUG=sw on Xvfb) support transfers into them. Without Vulkan 1.3 dynamic rendering the
 * screens are scaled on the CPU (nearest) into a staging buffer instead, with the same layouts.
 *
 * Layouts (video_option "layout", F9 cycles): stacked (top above bottom), side (side by side), large (big top screen,
 * small bottom screen at its lower right), top / bottom (one screen), auto: a small state machine picks "stacked"
 * in menus and the auto_gameplay layout (large, or top = bottom screen hidden) while flying:
 *     MENU -> GAMEPLAY   the circle pad has been held for 1.2 s (steering)
 *     GAMEPLAY -> MENU   Start or Select (pause menu), a click/touch on the bottom screen, or 8 s without steering
 * Aspect: keep (letterbox), integer (largest whole-number scale), stretch. The single layout's window opens 16:9
 * (window_aspect: another, e.g. 21:9 or 32:9; the view fills a wider window up to 32:9, compositor.cpp). Fullscreen: borderless (desktop) or
 * exclusive; F11 or Alt+Enter toggles. Mouse/touch input follows the bottom screen wherever the layout puts it.
 * Filters: nearest, linear, sharp bilinear, FXAA (present.frag), FSR 1 = AMD FidelityFX EASU upscale into a per-screen image
 * of the output size, then RCAS sharpening into the window (fsr_easu/fsr_rcas.frag, shaders/fsr/, MIT). F1 or Select opens
 * the settings overlay: the emulator-based build's menu (src/msr/overlay.cpp through msr/overlay_host.cpp), drawn last in
 * the presentation pass; 3D resolution and anisotropic filtering are passed to pica_vk.c. */
#include "gpu.h"
#include "input.h"
#include "msr/msr.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <SDL_syswm.h>
#include <vulkan/vulkan.h>
#include "msr/overlay_host.h"
#include "vk_push.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
extern int g_input_none;

#define TOP_W 400
#define BOT_W 320
#define SCR_H 240
#define MAX_IMAGES 8

static SDL_Window *win;
static VkInstance inst;
static VkSurfaceKHR surf;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkSwapchainKHR swap;
static VkImage sc_images[MAX_IMAGES];
static uint32_t sc_count;
static VkExtent2D sc_extent;
static VkFormat sc_format;
/* HDR output (hdr.frag): the swapchain is HDR10 or scRGB while the player has HDR on and the display is in HDR mode; the
   game is drawn into target as in SDR, the interface (HUD pieces, menus, the settings) into ui_layer, and hdr.frag
   combines them into the swapchain image */
static VkFormat sdr_format;          /* the SDR swapchain format: target and ui_layer have it */
static int hdr_mode;                 /* the swapchain now: 0 SDR, 1 HDR10 (PQ, BT.2020), 2 scRGB (linear BT.709) */
static int hdr_option;               /* 0 no HDR here, 1 the display is not in HDR mode, 2 HDR possible (msr_overlay_hdr) */
static float disp_peak = 1000.f, disp_paper = 200.f;   /* nits: the display's peak, Windows' SDR content white */
static int has_colorspace_ext, has_hdr_metadata;
static VkImageView sc_views[MAX_IMAGES];
static VkDeviceMemory stage_mem;
static VkBuffer stage;
static uint8_t *stage_ptr;
static VkDeviceSize stage_size;
static int sc_bgra;
static VkCommandPool pool;
static VkCommandBuffer cmdbuf;
static VkSemaphore sem_acquire, sem_done[MAX_IMAGES];   /* one per swapchain image: reused only after that image is re-acquired */
static VkFence fence;
static uint8_t top_px[TOP_W * SCR_H * 4], bot_px[BOT_W * SCR_H * 4];
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;   /* the queue is shared with the PICA renderer */
static int vk13;
unsigned g_vid_acquire_fail, g_vid_present_fail, g_vid_presented; int g_vid_last_err;

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "video: %s failed (%d)\n", #x, r_); return -1; } } while (0)

/* ---------------- options ---------------- */
enum { L_STACKED, L_SIDE, L_LARGE, L_TOP, L_BOTTOM, L_SINGLE, L_AUTO, L_COUNT };
static const char *layout_names[L_COUNT] = { "stacked", "side", "large", "top", "bottom", "single", "auto" };
enum { F_NEAREST, F_LINEAR, F_SHARP, F_FSR, F_FXAA, F_COUNT };
static const char *filter_names[F_COUNT] = { "nearest", "linear", "sharp", "fsr", "fxaa" };
static const char *aspect_names[3] = { "keep", "integer", "stretch" };
static const char *fs_names[3] = { "off", "borderless", "exclusive" };
static int opt_layout = L_STACKED, opt_filter = F_NEAREST, opt_aspect = 0, opt_fs = 0, opt_auto_gameplay = L_LARGE;
static int opt_res = 1, opt_aniso = 1, opt_fps = 0, opt_vsync = 1, opt_res_match = 0; static float opt_sharp = 0.25f;
static int opt_select_layout = 2;   /* the gamepad's Back/Select button: 0 3DS Select, 1 the next screen layout, 2 these settings (F1),
                                       as the emulator-based build's Select (the game takes Select as Start) */
static int opt_free_look = 0;       /* right stick orbits the chase camera (freelook.c) */
static int opt_drawdist = 1;        /* draw distance multiplier (drawdist.c), read when the game starts */
static int fs_last = 1;   /* the fullscreen mode F11 toggles to */
static float opt_window_aspect = 16.f / 9.f;   /* the single layout's window when it opens ("21:9", "2.37") */

static int pick(const char *v, const char *const *names, int n) { for (int i = 0; i < n; i++) if (!strcmp(v, names[i])) return i; return -1; }
int video_option(const char *name, const char *value) {
    int i;
    if (!strcmp(name, "layout") && (i = pick(value, layout_names, L_COUNT)) >= 0) opt_layout = i;
    else if (!strcmp(name, "filter") && (i = pick(value, filter_names, F_COUNT)) >= 0) opt_filter = i;
    else if (!strcmp(name, "aspect") && (i = pick(value, aspect_names, 3)) >= 0) opt_aspect = i;
    else if (!strcmp(name, "fullscreen") && (i = pick(value, fs_names, 3)) >= 0) { opt_fs = i; if (i) fs_last = i; }
    else if (!strcmp(name, "auto_gameplay") && (i = pick(value, layout_names, L_AUTO)) >= 0) opt_auto_gameplay = i;
    else if (!strcmp(name, "window_aspect")) {
        float a = 0, b = 1; if ((sscanf(value, "%f:%f", &a, &b) < 1) || b <= 0 || a / b < 1.f || a / b > 4.f) return -1;
        opt_window_aspect = a / b;
    }
    else if (!strcmp(name, "fsr_sharpness")) { char *e; double v = strtod(value, &e); if (*e || v < 0 || v > 2) return -1; opt_sharp = (float)v; }
    else if (!strcmp(name, "resolution")) {   /* 0: match the window (video_init, then each resize) */
        char *e; long v = strtol(value, &e, 10); if (e == value || *e || v < 0 || v > 10) return -1;
        opt_res_match = v == 0; if (v) { opt_res = (int)v; pica_vk_quality(opt_res, opt_aniso); }
    }
    else if (!strcmp(name, "aniso")) { i = atoi(value); if (i != 1 && i != 2 && i != 4 && i != 8 && i != 16) return -1; opt_aniso = i; pica_vk_quality(opt_res, opt_aniso); }
    else if (!strcmp(name, "show_fps")) opt_fps = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true");   /* the overlay draws it, from the settings */
    else if (!strcmp(name, "vsync")) opt_vsync = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true");
    else if (!strcmp(name, "select_layout")) opt_select_layout = !strcmp(value, "settings") || !strcmp(value, "2") ? 2 : !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true");
    else if (!strcmp(name, "draw_distance")) { i = atoi(value); if (i < 1 || i > 4) return -1; opt_drawdist = i; drawdist_set(i); }
    else if (!strcmp(name, "free_look")) { opt_free_look = !strcmp(value, "1") || !strcmp(value, "on") || !strcmp(value, "true"); freelook_enable(opt_free_look); }
    else return -1;
    return 0;
}

static void apply_fullscreen(void) {
#ifdef __ANDROID__
    return;   /* always the whole screen there (video_init) */
#endif
    SDL_SetWindowFullscreen(win, opt_fs == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : opt_fs == 2 ? SDL_WINDOW_FULLSCREEN : 0);
}

/* ---------------- layouts ---------------- */
typedef struct { float x, y, w, h; int on; } Rect;   /* window pixels */
static int auto_state;   /* 0 = menu, 1 = gameplay */

static int effective_layout(void) { return opt_layout == L_AUTO ? (auto_state ? opt_auto_gameplay : L_STACKED) : opt_layout; }

/* the layout's canvas in 3DS pixels, scaled into the window by the aspect mode */
static void layout_rects(int ww, int wh, Rect *top, Rect *bot) {
    if (effective_layout() == L_SINGLE) {   /* Samus Returns' 16:9 view; the bottom screen is a panel there only in menus */
        float t[4], p[4]; msr_single_layout((float)ww, (float)wh, t, p);
        *top = (Rect){ t[0], t[1], t[2], t[3], 1 };
        *bot = (Rect){ p[0], p[1], p[2], p[3], msr_panel_visible() };
        return;
    }
    float cw, ch; Rect t = { 0, 0, TOP_W, SCR_H, 1 }, b = { 40, SCR_H, BOT_W, SCR_H, 1 };
    switch (effective_layout()) {
    case L_SIDE:   cw = TOP_W + BOT_W; ch = SCR_H; b = (Rect){ TOP_W, 0, BOT_W, SCR_H, 1 }; break;
    case L_LARGE:  cw = TOP_W + BOT_W / 2; ch = SCR_H; b = (Rect){ TOP_W, SCR_H / 2, BOT_W / 2, SCR_H / 2, 1 }; break;
    case L_TOP:    cw = TOP_W; ch = SCR_H; b.on = 0; break;
    case L_BOTTOM: cw = BOT_W; ch = SCR_H; t.on = 0; b = (Rect){ 0, 0, BOT_W, SCR_H, 1 }; break;
    default:       cw = TOP_W; ch = 2 * SCR_H; break;
    }
    float sx = (float)ww / cw, sy = (float)wh / ch;
    if (opt_aspect != 2) { float s = sx < sy ? sx : sy; if (opt_aspect == 1 && s >= 1) s = floorf(s); sx = sy = s; }
    float ox = ((float)ww - cw * sx) / 2, oy = ((float)wh - ch * sy) / 2;
    *top = (Rect){ ox + t.x * sx, oy + t.y * sy, t.w * sx, t.h * sy, t.on };
    *bot = (Rect){ ox + b.x * sx, oy + b.y * sy, b.w * sx, b.h * sy, b.on };
}

/* auto layout: see the comment at the top */
static void auto_update(double now, int bottom_click) {
    static double stick_since = -1, last_stick; static uint32_t prev_btn;
    uint32_t btn = input_buttons(), pressed = btn & ~prev_btn; prev_btn = btn;
    int stick = input_stick_active();
    if (stick) last_stick = now;
    if (!auto_state) {
        if (!stick) stick_since = -1;
        else if (stick_since < 0) stick_since = now;
        else if (now - stick_since >= 1.2) auto_state = 1;
    } else if ((pressed & 0xC) || bottom_click || now - last_stick > 8.0) { auto_state = 0; stick_since = -1; }
}

/* ---------------- Vulkan setup ---------------- */
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; uint32_t w, h; } Img;
static int img_make(Img *im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(dev, &ci, NULL, &im->img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VK(vkAllocateMemory(dev, &ai, NULL, &im->mem)); VK(vkBindImageMemory(dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(dev, &vi, NULL, &im->view));
    im->w = w; im->h = h;
    return 0;
}
static void img_free(Img *im) {
    if (!im->img) return;
    vkDestroyImageView(dev, im->view, NULL); vkDestroyImage(dev, im->img, NULL); vkFreeMemory(dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* GPU presenter */
static int gpu_ok;
static Img tex_top, tex_bot, target;             /* screens, and the window-sized image drawn into */
static VkFormat target_fmt;
static VkSampler smp_linear;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout play;
static VkPipeline pipe, pipe_easu, pipe_rcas, pipe_rgba, pipe_msr;   /* pipe_msr: Samus Returns' compositor (msr.vert/msr.frag, play_msr) */
static VkPipeline pipe_ui, pipe_msr_ui;   /* the same into the HDR interface layer (coverage in alpha) */
static VkDescriptorSetLayout dsl_hdr; static VkPipelineLayout play_hdr; static VkPipeline pipe_hdr; static VkFormat pipe_hdr_fmt;
static Img ui_layer;   /* HDR: the interface, over the game (premultiplied, gamma-encoded) */
static VkPipelineLayout play_msr;   /* screens/overlay; FSR upscale into fsr_tmp; FSR sharpen; un-rotate into hi_scr */
static Img fsr_tmp[2], hi_scr[2], tex_toast;   /* FSR intermediates (top, bottom); the screens at the 3D internal resolution (pica_vk
                                                  scan-out, screen-oriented); the layout label */
int pica_vk_scanout(uint32_t addr, VkImageView *view, uint32_t *w, uint32_t *h);
void pica_vk_scan_lock(int on);
static VkBuffer up_buf; static VkDeviceMemory up_mem; static uint8_t *up_ptr;   /* uploads: screens, then the layout label */
/* the layout label, in 1x units: drawn at a whole-number scale k (1..4, font_ui.h) that suits the window and shown pixel
   for pixel, so text stays sharp at any window size; the pixel buffer holds the largest scale */
#define TOAST_W 240
#define TOAST_H 22
#define UI_KMAX 4
static uint8_t toast_px[TOAST_W * TOAST_H * 4 * UI_KMAX * UI_KMAX];   /* layout label (next_layout) */
#define UP_TOAST (sizeof top_px + sizeof bot_px)
static VkPushArena push_arena;   /* the stand-in descriptor sets of the frame (vk_push.c), reset at its fence */
typedef struct { float dst[4], src[4]; int mode[4]; float extra[4]; } Push;
typedef struct { float dst[4], src[4], style[4]; int flags[4]; float color[4]; } MsrPush;   /* msr.vert / msr.frag */

static const uint32_t present_vert[] =
#include "present.vert.inc"
;

static VkShaderModule shader(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(dev, &ci, NULL, &m); return m;
}

/* one pipeline per fragment shader: present.vert quad (or vs), alpha blending (the screens write alpha 1), dynamic viewport/scissor.
   blend 0: the colour target's alpha is replaced; 1: coverage accumulates in alpha (the HDR interface layer); 2: none */
static VkPipeline make_pipe_ex(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size, VkFormat fmt, VkPipelineLayout layout, int blend);
static VkPipeline make_pipe_with(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size, VkFormat fmt, VkPipelineLayout layout) {
    return make_pipe_ex(vs, vs_size, fs, fs_size, fmt, layout, 0);
}
static VkPipeline make_pipe_ex(const uint32_t *vs, size_t vs_size, const uint32_t *fs, size_t fs_size, VkFormat fmt, VkPipelineLayout layout, int blend) {
    VkShaderModule vm = shader(vs, vs_size), fm = shader(fs, fs_size);
    VkPipeline p = VK_NULL_HANDLE;
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" } };
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
        VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState ba = { .blendEnable = blend != 2, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = blend == 1 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
        VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
        VkGraphicsPipelineCreateInfo gci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pColorBlendState = &cb, .pDynamicState = &dy, .layout = layout };
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &p)) p = VK_NULL_HANDLE;
    }
    if (vm) vkDestroyShaderModule(dev, vm, NULL);
    if (fm) vkDestroyShaderModule(dev, fm, NULL);
    return p;
}
static VkPipeline make_pipe(const uint32_t *fs, size_t fs_size, VkFormat fmt) { return make_pipe_with(present_vert, sizeof present_vert, fs, fs_size, fmt, play); }

static int gpu_init(void) {
    if (!vk13) return -1;
    if (img_make(&tex_top, TOP_W, SCR_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        img_make(&tex_bot, BOT_W, SCR_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return -1;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = UP_TOAST + sizeof toast_px, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VK(vkCreateBuffer(dev, &bi, NULL, &up_buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, up_buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(dev, &mai, NULL, &up_mem)); VK(vkBindBufferMemory(dev, up_buf, up_mem, 0));
    VK(vkMapMemory(dev, up_mem, 0, bi.size, 0, (void **)&up_ptr));
    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VK(vkCreateSampler(dev, &si, NULL, &smp_linear));
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = vk_push_layout_flags(),
        .bindingCount = 1, .pBindings = &b };
    VK(vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl));
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    VK(vkCreatePipelineLayout(dev, &pli, NULL, &play));
    vk_push_layout(play, dsl);
    target_fmt = sdr_format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : sdr_format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : sdr_format;
    static const uint32_t fs_present[] =
#include "present.frag.inc"
    ;
    static const uint32_t fs_easu[] =
#include "fsr_easu.frag.inc"
    ;
    static const uint32_t fs_rcas[] =
#include "fsr_rcas.frag.inc"
    ;
    pipe = make_pipe(fs_present, sizeof fs_present, target_fmt);
    pipe_easu = make_pipe(fs_easu, sizeof fs_easu, VK_FORMAT_R8G8B8A8_UNORM);
    pipe_rcas = make_pipe(fs_rcas, sizeof fs_rcas, target_fmt);
    pipe_rgba = make_pipe(fs_present, sizeof fs_present, VK_FORMAT_R8G8B8A8_UNORM);
    if (!pipe || !pipe_easu || !pipe_rcas || !pipe_rgba) return -1;
    {   /* Samus Returns' compositor: its own push constants (MsrPush) */
        static const uint32_t vs_msr[] =
#include "msr.vert.inc"
        ;
        static const uint32_t fs_msr[] =
#include "msr.frag.inc"
        ;
        VkPushConstantRange mr2 = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MsrPush) };
        VkPipelineLayoutCreateInfo ml = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &mr2 };
        VK(vkCreatePipelineLayout(dev, &ml, NULL, &play_msr));
        vk_push_layout(play_msr, dsl);
        pipe_msr = make_pipe_with(vs_msr, sizeof vs_msr, fs_msr, sizeof fs_msr, target_fmt, play_msr);
        pipe_msr_ui = make_pipe_ex(vs_msr, sizeof vs_msr, fs_msr, sizeof fs_msr, target_fmt, play_msr, 1);
        if (!pipe_msr || !pipe_msr_ui) return -1;
    }
    pipe_ui = make_pipe_ex(present_vert, sizeof present_vert, fs_present, sizeof fs_present, target_fmt, play, 1);
    {   /* HDR: two images (game, interface), present.vert's push constants; the pipeline is made for the swapchain's format */
        VkDescriptorSetLayoutBinding hb[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                                               { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
        VkDescriptorSetLayoutCreateInfo hci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = vk_push_layout_flags(),
            .bindingCount = 2, .pBindings = hb };
        VK(vkCreateDescriptorSetLayout(dev, &hci, NULL, &dsl_hdr));
        VkPipelineLayoutCreateInfo hl = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl_hdr, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
        VK(vkCreatePipelineLayout(dev, &hl, NULL, &play_hdr));
        vk_push_layout(play_hdr, dsl_hdr);
    }
    if (!pipe_ui) return -1;
    /* tex_toast: made at its first use, at the scale of that moment (ui_img) */
    return 0;
}

static int target_fps = 60;
static int recreate_swapchain(void);
static void match_resolution(int drawable_w, int drawable_h);
/* above 60 FPS the main loop paces VBlank itself: present without waiting for the display (mailbox, else FIFO); without
   V-Sync, mailbox or else immediate (tearing) */
static VkPresentModeKHR present_mode(void) {
    if (target_fps <= 60 && opt_vsync) return VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR modes[16]; uint32_t n = 16;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surf, &n, modes) != VK_SUCCESS && n == 0) return VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) return VK_PRESENT_MODE_MAILBOX_KHR;
    if (!opt_vsync) for (uint32_t i = 0; i < n; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}
/* the frame rate (also while playing: the overlay's Frame rate) */
void video_set_fps(int fps) {
    int before = swap ? (int)present_mode() : -1;
    target_fps = fps;
    if (swap && before != (int)present_mode()) recreate_swapchain();
}

/* the surface's formats: SDR (B8G8R8A8_UNORM, sRGB colour space, else the first), and the HDR ones (0 when absent) */
static void surface_formats(VkFormat *sdr, VkColorSpaceKHR *sdr_cs, VkFormat *hdr10, VkFormat *scrgb) {
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, NULL);
    VkSurfaceFormatKHR fmts[64]; if (nf > 64) nf = 64; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, fmts);
    *sdr = fmts[0].format; *sdr_cs = fmts[0].colorSpace; *hdr10 = *scrgb = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < nf; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { *sdr = fmts[i].format; *sdr_cs = fmts[i].colorSpace; }
        if (fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && (fmts[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || fmts[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)) *hdr10 = fmts[i].format;
        if (fmts[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && fmts[i].format == VK_FORMAT_R16G16B16A16_SFLOAT) *scrgb = fmts[i].format;
    }
}

int win_hdr_info(void *hwnd, float *peak, float *paper);   /* win/hdr_win.c */
/* whether HDR output is possible on the window's display (at start, and when the settings open: Windows' "Use HDR" may
   have changed), told to the settings overlay */
static void hdr_detect(void) {
    VkFormat sdr, h10, scrgb; VkColorSpaceKHR cs; surface_formats(&sdr, &cs, &h10, &scrgb);
    int formats = has_colorspace_ext && (h10 || scrgb), opt = 0; float peak = 1000.f, paper = 200.f;
#ifdef _WIN32
    SDL_SysWMinfo wi; SDL_VERSION(&wi.version);
    int on = SDL_GetWindowWMInfo(win, &wi) && win_hdr_info(wi.info.win.window, &peak, &paper);
    opt = formats && on ? 2 : 1;
#else
    opt = formats ? 2 : 0;
#endif
    if (opt != hdr_option || (peak < 1000.f ? peak : 1000.f) != disp_peak || paper != disp_paper) {
        fprintf(stderr, "video: HDR %s (display peak %.0f nits, SDR white %.0f nits; swapchain HDR10 %s, scRGB %s)\n",
                opt == 2 ? "possible" : opt == 1 ? "off on this display" : "not available", peak, paper, h10 ? "yes" : "no", scrgb ? "yes" : "no");
        /* the highlights' default stays at 1000 nits however bright the display (a 3,900-nit glow is blinding); the menu goes higher */
        hdr_option = opt; disp_peak = peak < 1000.f ? peak : 1000.f; disp_paper = paper;
        msr_overlay_hdr(opt, disp_peak, paper);
    }
}
/* HDR wanted now: the player's setting, on a display in HDR mode */
static int hdr_wanted(void) { float pk, pp; return msr_hdr(&pk, &pp) && hdr_option == 2 && gpu_ok && vk13; }

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { fprintf(stderr, "video: swapchain cannot be a copy target\n"); return -1; }
    VkFormat h10, scrgb; VkColorSpaceKHR cs;
    surface_formats(&sc_format, &cs, &h10, &scrgb);
    hdr_mode = 0;
    if (hdr_wanted() && (h10 || scrgb)) {
        hdr_mode = h10 ? 1 : 2; sc_format = h10 ? h10 : scrgb; cs = h10 ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
    }
    sc_extent = caps.currentExtent;
    if (sc_extent.width == UINT32_MAX) { int w, h; SDL_Vulkan_GetDrawableSize(win, &w, &h); sc_extent.width = (uint32_t)w; sc_extent.height = (uint32_t)h; }
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
#ifdef __ANDROID__
    /* a phone held sideways has a 90 degree transform (its panel is portrait): the picture stays upright in landscape
       (identity), sized as the window is (drivers differ on which way round they report the size), and the compositor
       turns it */
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    { int w, h; SDL_Vulkan_GetDrawableSize(win, &w, &h); if (w > 0 && h > 0) { sc_extent.width = (uint32_t)w; sc_extent.height = (uint32_t)h; } }
#endif
    if (!sc_extent.width || !sc_extent.height) return 0;   /* minimised: try again next frame */
    /* opaque, or what the surface has (Android's often only inherit) */
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1)
            if (caps.supportedCompositeAlpha & bit) { alpha = (VkCompositeAlphaFlagBitsKHR)bit; break; }
    uint32_t n = caps.minImageCount + 1; if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = n,
        .imageFormat = sc_format, .imageColorSpace = cs, .imageExtent = sc_extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = transform, .compositeAlpha = alpha,
        .presentMode = present_mode(), .clipped = VK_TRUE, .oldSwapchain = swap };
    VkSwapchainKHR old = swap;
    VK(vkCreateSwapchainKHR(dev, &ci, NULL, &swap));
    if (old) vkDestroySwapchainKHR(dev, old, NULL);
    sc_count = MAX_IMAGES; VK(vkGetSwapchainImagesKHR(dev, swap, &sc_count, sc_images));
    sc_bgra = sc_format == VK_FORMAT_B8G8R8A8_UNORM || sc_format == VK_FORMAT_B8G8R8A8_SRGB;
    for (int i = 0; i < MAX_IMAGES; i++) if (sc_views[i]) { vkDestroyImageView(dev, sc_views[i], NULL); sc_views[i] = VK_NULL_HANDLE; }
    if (gpu_ok) {   /* the window-sized image the presenter draws into */
        img_free(&target); img_free(&ui_layer);
        if (img_make(&target, sc_extent.width, sc_extent.height, target_fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return -1;
        if (hdr_mode) {   /* HDR: the interface layer, views of the swapchain images (hdr.frag draws into them), the pipeline for this format */
            if (img_make(&ui_layer, sc_extent.width, sc_extent.height, target_fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return -1;
            for (uint32_t i = 0; i < sc_count; i++) {
                VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = sc_images[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sc_format,
                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                VK(vkCreateImageView(dev, &vi, NULL, &sc_views[i]));
            }
            if (!pipe_hdr || pipe_hdr_fmt != sc_format) {
                static const uint32_t fs_hdr[] =
#include "hdr.frag.inc"
                ;
                if (pipe_hdr) vkDestroyPipeline(dev, pipe_hdr, NULL);
                pipe_hdr = make_pipe_ex(present_vert, sizeof present_vert, fs_hdr, sizeof fs_hdr, sc_format, play_hdr, 2); pipe_hdr_fmt = sc_format;
                if (!pipe_hdr) { fprintf(stderr, "video: HDR pipeline failed\n"); return -1; }
            }
            if (has_hdr_metadata) {   /* what the game's picture reaches: the display tone-maps the rest */
                PFN_vkSetHdrMetadataEXT set = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(dev, "vkSetHdrMetadataEXT");
                float pk, pp; msr_hdr(&pk, &pp); if (pk <= 0) pk = disp_peak; if (pp <= 0) pp = disp_paper;
                VkHdrMetadataEXT md = { .sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                    .displayPrimaryRed = { 0.708f, 0.292f }, .displayPrimaryGreen = { 0.170f, 0.797f }, .displayPrimaryBlue = { 0.131f, 0.046f },
                    .whitePoint = { 0.3127f, 0.3290f }, .maxLuminance = pk, .minLuminance = 0.f, .maxContentLightLevel = pk, .maxFrameAverageLightLevel = pp };
                if (set) set(dev, 1, &swap, &md);
            }
            fprintf(stderr, "video: HDR output (%s)\n", hdr_mode == 1 ? "HDR10" : "scRGB");
        }
        return 0;
    }
    /* CPU path: staging buffer sized to the swapchain */
    if (stage) { vkUnmapMemory(dev, stage_mem); vkDestroyBuffer(dev, stage, NULL); vkFreeMemory(dev, stage_mem, NULL); stage = VK_NULL_HANDLE; }
    stage_size = (VkDeviceSize)sc_extent.width * sc_extent.height * 4;
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = stage_size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VK(vkCreateBuffer(dev, &bi, NULL, &stage));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, stage, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VK(vkAllocateMemory(dev, &mai, NULL, &stage_mem)); VK(vkBindBufferMemory(dev, stage, stage_mem, 0));
    VK(vkMapMemory(dev, stage_mem, 0, stage_size, 0, (void **)&stage_ptr));
    return 0;
}

static int recreate_swapchain(void) {
    pthread_mutex_lock(&queue_lock); vkDeviceWaitIdle(dev); pthread_mutex_unlock(&queue_lock);
    return create_swapchain();
}
/* the window's surface again, and its swapchain: Android destroys the window's surface while the app is in the
   background (VK_ERROR_SURFACE_LOST_KHR, or the foreground again: SDL_APP_DIDENTERFOREGROUND) */
static int surface_reset;
static int recreate_surface(void) {
    pthread_mutex_lock(&queue_lock); vkDeviceWaitIdle(dev); pthread_mutex_unlock(&queue_lock);
    if (swap) { vkDestroySwapchainKHR(dev, swap, NULL); swap = VK_NULL_HANDLE; }
    vkDestroySurfaceKHR(inst, surf, NULL); surf = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(win, inst, &surf)) { fprintf(stderr, "video: surface: %s\n", SDL_GetError()); return -1; }
    VkBool32 pres = 0; vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, surf, &pres);
    fprintf(stderr, "video: the window's surface again (presentable %d)\n", (int)pres);
    return create_swapchain();
}

int video_init(int scale) {
    /* every pad's face buttons by position (A = bottom), as input.c, glyphs.cpp and the settings take them: SDL 2 names a
       Nintendo pad's by label unless told (its A, on the right, came as SDL's A, the bottom button) */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
#ifdef __APPLE__
    /* MoltenVK: its errors only in the log (not its device report, nor a warning for each pipeline that Metal always
       restarts primitives); MVK_CONFIG_LOG_LEVEL set by the player wins */
    setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | (g_input_none ? 0 : SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK))) { fprintf(stderr, "video: SDL_Init: %s\n", SDL_GetError()); return -1; }
    static const int cw[L_COUNT] = { TOP_W, TOP_W + BOT_W, TOP_W + BOT_W / 2, TOP_W, BOT_W, 0, TOP_W }, chh[L_COUNT] = { 2 * SCR_H, SCR_H, SCR_H, SCR_H, SCR_H, SCR_H, 2 * SCR_H };
    int win_w = opt_layout == L_SINGLE ? (int)lroundf(SCR_H * scale * opt_window_aspect) : cw[opt_layout] * scale;   /* single: 16:9 (1280x720 at 3) */
#ifdef __ANDROID__
    const Uint32 win_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN;   /* the whole screen, without the system bars */
#elif defined(__APPLE__)
    /* a Retina display's own pixels (the drawable is then larger than the window; input and the overlay map between them) */
    const Uint32 win_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    const Uint32 win_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
#endif
    win = SDL_CreateWindow("Metroid: Samus Returns", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, chh[opt_layout] * scale, win_flags);
    if (!win) { fprintf(stderr, "video: SDL_CreateWindow: %s\n", SDL_GetError()); return -1; }
    SDL_SetWindowMinimumSize(win, BOT_W, SCR_H);
    if (opt_fs) apply_fullscreen();
    unsigned ne = 0; SDL_Vulkan_GetInstanceExtensions(win, &ne, NULL);
    const char *exts[20]; if (ne > 16) ne = 16; SDL_Vulkan_GetInstanceExtensions(win, &ne, exts);
    int has_portability_enum = 0;
    {   /* HDR swapchain colour spaces */
        uint32_t n = 0; vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties *ep = calloc(n ? n : 1, sizeof *ep); vkEnumerateInstanceExtensionProperties(NULL, &n, ep);
        for (uint32_t i = 0; i < n; i++) if (!strcmp(ep[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) has_colorspace_ext = 1;
#ifdef __APPLE__
        /* MoltenVK is a portability driver: through the Vulkan loader it is listed only when asked for */
        for (uint32_t i = 0; i < n; i++) if (!strcmp(ep[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) has_portability_enum = 1;
#endif
        free(ep);
        if (has_colorspace_ext) exts[ne++] = VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME;
        if (has_portability_enum) exts[ne++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    }
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "msr-native", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = ne, .ppEnabledExtensionNames = exts,
        .flags = has_portability_enum ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0 };
    VK(vkCreateInstance(&ici, NULL, &inst));
    if (!SDL_Vulkan_CreateSurface(win, inst, &surf)) { fprintf(stderr, "video: surface: %s\n", SDL_GetError()); return -1; }
    uint32_t np = 8; VkPhysicalDevice pds[8]; VK(vkEnumeratePhysicalDevices(inst, &np, pds));
    for (uint32_t i = 0; i < np && !phys; i++) {
        uint32_t nq = 16; VkQueueFamilyProperties qp[16]; vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 pres = 0; vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surf, &pres);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) { phys = pds[i]; qfam = q; break; }
        }
    }
    if (!phys) { fprintf(stderr, "video: no Vulkan device can present to the window\n"); return -1; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp);
    fprintf(stderr, "video: Vulkan device %s\n", pp.deviceName);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    /* the hardware PICA renderer (pica_vk.c) and the GPU presenter need Vulkan 1.3 dynamic rendering + extended dynamic state and push descriptors */
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
    uint32_t nde = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, NULL);
    VkExtensionProperties *dep = calloc(nde ? nde : 1, sizeof *dep); vkEnumerateDeviceExtensionProperties(phys, NULL, &nde, dep);
    int has_push = 0; for (uint32_t i = 0; i < nde; i++) if (!strcmp(dep[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) has_push = 1;
    int has_portability_subset = 0;   /* MoltenVK: a device that has it must enable it */
    for (uint32_t i = 0; i < nde; i++) if (!strcmp(dep[i].extensionName, "VK_KHR_portability_subset")) has_portability_subset = 1;
    for (uint32_t i = 0; i < nde; i++) if (!strcmp(dep[i].extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME)) has_hdr_metadata = 1;
    free(dep);
    if (pp.apiVersion >= VK_API_VERSION_1_3) vkGetPhysicalDeviceFeatures2(phys, &f2);
    /* push descriptors where the device has them, else descriptor sets in their place (vk_push.c) */
    vk13 = pp.apiVersion >= VK_API_VERSION_1_3 && f13.dynamicRendering && f13.shaderDemoteToHelperInvocation;
    VkPhysicalDeviceVulkan13Features en13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .shaderDemoteToHelperInvocation = VK_TRUE };
    VkPhysicalDeviceFeatures2 en2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &en13 };
    en2.features.samplerAnisotropy = f2.features.samplerAnisotropy;   /* pica_vk.c: anisotropic filtering option */
    const char *dext[4] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME }; uint32_t ndext = 1;
    if (has_portability_subset) dext[ndext++] = "VK_KHR_portability_subset";
    if (vk13 && has_push) dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
    if (vk13 && has_hdr_metadata) dext[ndext++] = VK_EXT_HDR_METADATA_EXTENSION_NAME; else has_hdr_metadata = 0;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = vk13 ? &en2 : NULL, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = ndext, .ppEnabledExtensionNames = dext };
    if (!vk13) fprintf(stderr, "video: no Vulkan 1.3 dynamic rendering: hardware PICA renderer and GPU presenter unavailable "
                       "(Vulkan %u.%u, dynamic rendering %d, demote to helper %d)\n",
                       VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), (int)f13.dynamicRendering,
                       (int)f13.shaderDemoteToHelperInvocation);
    VK(vkCreateDevice(phys, &dci, NULL, &dev));
    if (vk13) vk_push_init(dev, has_push);
#ifdef MSR_VK13_FROM_DEVICE
    { extern void msr_vk13_load(VkDevice d); if (vk13) msr_vk13_load(dev); }   /* vk13_load.c */
#endif
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    /* swapchain format first (the presenter's target uses it), then the presenter, then the swapchain-sized resources */
    { VkFormat h10, scrgb; VkColorSpaceKHR cs; surface_formats(&sdr_format, &cs, &h10, &scrgb); sc_format = sdr_format; }
    gpu_ok = !gpu_init();
    hdr_detect();
    if (!gpu_ok) fprintf(stderr, "video: GPU presenter unavailable, scaling on the CPU (nearest)\n");
    if (create_swapchain()) return -1;
    if (opt_res_match) { int w, h; SDL_Vulkan_GetDrawableSize(win, &w, &h); match_resolution(w, h); }   /* --resolution=0 */

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfam };
    VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VK(vkAllocateCommandBuffers(dev, &cai, &cmdbuf));
    VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK(vkCreateSemaphore(dev, &si, NULL, &sem_acquire)); for (int i = 0; i < MAX_IMAGES; i++) VK(vkCreateSemaphore(dev, &si, NULL, &sem_done[i]));
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VK(vkCreateFence(dev, &fi, NULL, &fence));
    if (gpu_ok) {   /* the settings overlay draws into the presentation's target (its font goes up on the shared queue) */
        pthread_mutex_lock(&queue_lock);
        msr_overlay_vk_init(inst, phys, dev, qfam, queue, target_fmt, sc_count);
        pthread_mutex_unlock(&queue_lock);
    }
    return 0;
}

/* LCD framebuffers are rotated: each line holds 240 pixels running bottom-to-top of one screen column */
static void decode_screen(const ScreenSource *s, int w, uint8_t *out) {
    static const int bpp_of[8] = { 4, 3, 2, 2, 2, 0, 0, 0 };
    int bpp = bpp_of[s->format & 7];
    if (!s->valid || !s->addr || !bpp) {   /* no scan-out source yet: dark grey, so screenshots show the screen area */
        for (int i = 0; i < w * SCR_H; i++) { out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = 40; out[4 * i + 3] = 255; }
        return;
    }
    uint32_t stride = s->stride ? s->stride : 240u * (uint32_t)bpp;
    for (int x = 0; x < w; x++) for (int y = 0; y < SCR_H; y++) {
        const uint8_t *p = g_mem + s->addr + (uint32_t)x * stride + (uint32_t)(239 - y) * (uint32_t)bpp;
        uint8_t *d = out + ((size_t)y * (size_t)w + (size_t)x) * 4; uint32_t v;
        switch (s->format & 7) {
        case 0: d[0] = p[3]; d[1] = p[2]; d[2] = p[1]; break;
        case 1: d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; break;
        case 2: v = p[0] | p[1] << 8; d[0] = (uint8_t)((v >> 11) << 3); d[1] = (uint8_t)(((v >> 5) & 63) << 2); d[2] = (uint8_t)((v & 31) << 3); break;
        case 3: v = p[0] | p[1] << 8; d[0] = (uint8_t)((v >> 11) << 3); d[1] = (uint8_t)(((v >> 6) & 31) << 3); d[2] = (uint8_t)(((v >> 1) & 31) << 3); break;
        case 4: v = p[0] | p[1] << 8; d[0] = (uint8_t)((v >> 12) * 17); d[1] = (uint8_t)(((v >> 8) & 15) * 17); d[2] = (uint8_t)(((v >> 4) & 15) * 17); break;
        }
        d[3] = 255;
    }
}

static void barrier_img(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---------------- the settings overlay's display settings, the layout label ---------------- */
#include "font_ui.h"
/* text and boxes in pixels of a label drawn at scale k (cells of 8k x 16k) */
static void text(uint8_t *px, int pw, int ph, int x, int y, const char *str, uint32_t rgb, int k) {
    for (; *str; str++, x += 8 * k) {
        int c = (unsigned char)*str; if (c < 32 || c > 126) c = '?';
        const unsigned char *g = font_ui_glyph(k, c);
        for (int gy = 0; gy < 16 * k; gy++) for (int gx = 0; gx < 8 * k; gx++) {
            int X = x + gx, Y = y + gy; unsigned a = g[gy * 8 * k + gx];
            if (!a || X < 0 || Y < 0 || X >= pw || Y >= ph) continue;
            uint8_t *d = px + ((size_t)Y * (size_t)pw + (size_t)X) * 4;
            for (int ch = 0; ch < 3; ch++) d[ch] = (uint8_t)((d[ch] * (255 - a) + ((rgb >> (16 - 8 * ch)) & 255) * a) / 255);
            if (d[3] < a) d[3] = (uint8_t)a;
        }
    }
}
static void fill(uint8_t *px, int pw, int x0, int y0, int w, int h, uint32_t rgb, int a) {
    for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) {
        uint8_t *d = px + ((size_t)y * (size_t)pw + (size_t)x) * 4; d[0] = (uint8_t)(rgb >> 16); d[1] = (uint8_t)(rgb >> 8); d[2] = (uint8_t)rgb; d[3] = (uint8_t)a;
    }
}

void video_set_fullscreen(int on) { opt_fs = on ? fs_last : 0; apply_fullscreen(); }
/* the launcher (msr/launcher.cpp): its window hides while the game runs; the file dialog's owner */
void video_show(int on) { if (!win) return; if (on) { SDL_ShowWindow(win); SDL_RaiseWindow(win); } else SDL_HideWindow(win); }
SDL_Window *video_window(void) { return win; }
void video_set_vsync(int on) { opt_vsync = on != 0; if (swap) recreate_swapchain(); }
/* the 3D resolution: 1..10, or 0 to follow the window: the 240-line multiple nearest the top screen's height as shown
   (FSR takes the rest). By the window's whole height, rounded up, a 1280x800 Steam Deck got 4x: 1600x960 drawn for a
   view of 720 lines (16:9) or 768 (the 3DS's 5:3); 3x now. */
static void match_resolution(int drawable_w, int drawable_h) {
    Rect t, b; layout_rects(drawable_w, drawable_h, &t, &b);
    int lines = t.on && t.h >= 1 ? (int)(t.h + 0.5f) : drawable_h;
    int r = (lines + SCR_H / 2) / SCR_H; r = r < 1 ? 1 : r > 10 ? 10 : r;
    if (r != opt_res) { opt_res = r; pica_vk_quality(opt_res, opt_aniso); }
}
void video_set_resolution(int factor) {
    opt_res_match = factor <= 0;
    if (opt_res_match) { int w, h; SDL_Vulkan_GetDrawableSize(win, &w, &h); match_resolution(w, h); return; }
    opt_res = factor > 10 ? 10 : factor; pica_vk_quality(opt_res, opt_aniso);
}
int video_resolution(void) { return opt_res_match ? 0 : opt_res; }
int video_fullscreen(void) { return opt_fs != 0; }
int video_vsync(void) { return opt_vsync; }
/* every gamepad SDL can map is open (Steam can show a pad twice, a player can switch pads); gc is the one used last: its
   sticks are read, its type names the buttons (msr/glyphs.cpp) */
#define MAX_PADS 4
static SDL_GameController *gcs[MAX_PADS], *gc; static SDL_Joystick *joy;
int video_pad_connected(void) { return gc || joy; }
static SDL_GameController *pad_of(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++) if (gcs[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gcs[i])) == id) return gcs[i];
    return NULL;
}
static void pad_used(SDL_GameController *c) {   /* this pad was used: its sticks are read, its type names the buttons */
    if (!c) return;
    gc = c;
    msr_input_device(1, (int)SDL_GameControllerGetType(c));
}
int video_pad_any_held(void) {   /* for a new binding: the button that chose the row comes up first */
    if (gc) for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) if (SDL_GameControllerGetButton(gc, (SDL_GameControllerButton)b)) return 1;
    return 0;
}

/* gamepads: only without --input=none. Controllers with an SDL mapping use the controller API; others fall back to raw joystick
   buttons/axes/hat (0..3 = south,east,west,north as most pads number them, 4/5 = L/R, 6/7 = select/start) */
static void pad_open(int idx) {
    if (SDL_IsGameController(idx)) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(idx);
        if (pad_of(id)) return;
        for (int i = 0; i < MAX_PADS; i++) if (!gcs[i]) {
            if (!(gcs[i] = SDL_GameControllerOpen(idx))) return;
            static const char *const types[] = { "unknown", "Xbox 360", "Xbox One", "PS3", "PS4", "Nintendo Switch Pro", "virtual", "PS5",
                                                 "Amazon Luna", "Google Stadia", "NVIDIA Shield", "Joy-Con (L)", "Joy-Con (R)", "Joy-Con pair" };
            int t = (int)SDL_GameControllerGetType(gcs[i]);
            fprintf(stderr, "pwr: controller opened: %s (%s)\n", SDL_GameControllerName(gcs[i]), t >= 0 && t < (int)(sizeof types / sizeof *types) ? types[t] : "?");
            if (!gc) pad_used(gcs[i]);   /* the first one names the buttons until another is used */
            return;
        }
        return;
    }
    if (gc || joy) return;
    if ((joy = SDL_JoystickOpen(idx))) fprintf(stderr, "pwr: controller opened: %s (joystick, %d buttons, %d axes, %d hats)\n", SDL_JoystickName(joy), SDL_JoystickNumButtons(joy), SDL_JoystickNumAxes(joy), SDL_JoystickNumHats(joy));
}
static void pad_axes(void) {
    int x, y, rx, ry;
    if (gc) {
        x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX); y = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX); ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
    } else if (joy) {   /* raw pads: axes 0/1 left stick; right stick 3/4 on six-axis pads (2 is a trigger), else 2/3 */
        int na = SDL_JoystickNumAxes(joy), r0 = na >= 6 ? 3 : 2;
        x = na > 0 ? SDL_JoystickGetAxis(joy, 0) : 0; y = na > 1 ? -SDL_JoystickGetAxis(joy, 1) : 0;
        rx = na > r0 ? SDL_JoystickGetAxis(joy, r0) : 0; ry = na > r0 + 1 ? -SDL_JoystickGetAxis(joy, r0 + 1) : 0;
    } else return;
    input_pad_stick(x, y);
    input_pad_rstick(rx, ry);   /* the D-pad when the Aeion abilities are on the right stick (input.c) */
    freelook_stick(rx, ry);
}

/* next screen layout (F9, the gamepad's Back/Select): a label shows the new layout for 1.5 s, and the choice is saved */
static double toast_until; static int toast_dirty; static const char *toast_text;
static double now_s(void);
static void next_layout(void) {
    static const char *nice[L_COUNT] = { "Layout: stacked", "Layout: side by side", "Layout: large top", "Layout: top only", "Layout: bottom only", "Layout: single screen", "Layout: auto" };
    opt_layout = (opt_layout + 1) % L_COUNT;
    toast_text = nice[opt_layout];   /* drawn at the next present, at the UI scale */
    toast_until = now_s() + 1.5; toast_dirty = 1;
    fprintf(stderr, "pwr: layout %s\n", layout_names[opt_layout]);
}

/* hotkeys (never passed to the game): F11 / Alt+Enter fullscreen, F9 next layout. Returns 1 when the key was a hotkey. */
static int hotkey(const SDL_KeyboardEvent *k) {
    if (k->type != SDL_KEYDOWN || k->repeat) return k->keysym.sym == SDLK_F11 || k->keysym.sym == SDLK_F9 || k->keysym.sym == SDLK_F5 || k->keysym.sym == SDLK_F8 || (k->keysym.sym == SDLK_RETURN && (k->keysym.mod & KMOD_ALT));
    if (k->keysym.sym == SDLK_F5) { msr_state_quick_save(); return 1; }   /* the settings' Quick save and Quick load */
    if (k->keysym.sym == SDLK_F8) { msr_state_quick_load(); return 1; }
    if (k->keysym.sym == SDLK_F11 || (k->keysym.sym == SDLK_RETURN && (k->keysym.mod & KMOD_ALT))) {
        opt_fs = opt_fs ? 0 : fs_last; apply_fullscreen(); msr_overlay_fullscreen(opt_fs != 0); return 1;
    }
    if (k->keysym.sym == SDLK_F9) { next_layout(); return 1; }
    return 0;
}

/* CPU path: nearest scale of one screen into its rectangle of the staging buffer */
static void cpu_blit(const uint8_t *src, int sw, Rect r) {
    if (!r.on) return;
    int x0 = (int)r.x, y0 = (int)r.y, w = (int)r.w, h = (int)r.h;
    for (int y = 0; y < h; y++) {
        int dy = y0 + y; if (dy < 0 || dy >= (int)sc_extent.height) continue;
        const uint8_t *srow = src + (size_t)(y * SCR_H / h) * (size_t)sw * 4;
        uint8_t *drow = stage_ptr + (size_t)dy * sc_extent.width * 4;
        for (int x = 0; x < w; x++) {
            int dx = x0 + x; if (dx < 0 || dx >= (int)sc_extent.width) continue;
            const uint8_t *p = srow + (size_t)(x * sw / w) * 4; uint8_t *d = drow + (size_t)dx * 4;
            if (sc_bgra) { d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; } else { d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; }
            d[3] = 255;
        }
    }
}

/* GPU path: draw a texture into a rectangle of the current attachment (W x H) with a pipeline */
static void gpu_quad_view(VkPipeline pl, VkImageView view, uint32_t tw, uint32_t th, VkImageLayout lay, int rot, Rect r, float W, float H, int mode, int alpha, const float extra[4]) {
    if (!r.on || r.w < 1 || r.h < 1) return;
    Push p = { { r.x / W * 2 - 1, r.y / H * 2 - 1, (r.x + r.w) / W * 2 - 1, (r.y + r.h) / H * 2 - 1 },
               { (float)tw, (float)th, r.w / (float)tw, r.h / (float)th }, { mode, alpha, rot, 0 }, { 0, 0, 0, 0 } };
    if (extra) memcpy(p.extra, extra, sizeof p.extra);
    VkDescriptorImageInfo ii = { smp_linear, view, lay };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
    vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    vk_push(&push_arena, cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, play, 0, 1, &w);
    vkCmdPushConstants(cmdbuf, play, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(cmdbuf, 4, 1, 0, 0);
}
/* Samus Returns' compositor: one quad of msr_compose's list, sampling the bottom screen */
static void msr_quad(const MsrQuad *q, const Img *bottom, float W, float H) {
    if (q->dst[2] <= 0 || q->dst[3] <= 0) return;
    MsrPush p = { { q->dst[0] / W * 2 - 1, q->dst[1] / H * 2 - 1, (q->dst[0] + q->dst[2]) / W * 2 - 1, (q->dst[1] + q->dst[3]) / H * 2 - 1 },
                  { q->src[0], q->src[1], q->src[2], q->src[3] }, { q->style[0], q->style[1], q->style[2], q->style[3] },
                  { q->feather, q->circle, 0, 0 }, { q->color[0], q->color[1], q->color[2], q->color[3] } };
    VkDescriptorImageInfo ii = { smp_linear, bottom->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
    vk_push(&push_arena, cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, play_msr, 0, 1, &w);
    vkCmdPushConstants(cmdbuf, play_msr, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    vkCmdDraw(cmdbuf, 4, 1, 0, 0);
}
static void gpu_quad(VkPipeline pl, const Img *t, Rect r, float W, float H, int mode, int alpha, const float extra[4]) {
    gpu_quad_view(pl, t->view, t->w, t->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, r, W, H, mode, alpha, extra);
}
static void begin_pass_a(const Img *att, VkAttachmentLoadOp load, float clear_alpha) {
    barrier_img(att->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = att->view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = load, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0, clear_alpha } } } };
    VkExtent2D ex = { att->w, att->h };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, ex }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmdbuf, &ri);
    VkViewport vp = { 0, 0, (float)att->w, (float)att->h, 0, 1 }; VkRect2D sc = { { 0, 0 }, ex };
    vkCmdSetViewport(cmdbuf, 0, 1, &vp); vkCmdSetScissor(cmdbuf, 0, 1, &sc);
}
static void begin_pass(const Img *att, VkAttachmentLoadOp load) { begin_pass_a(att, load, 1); }

/* FSR: EASU upscale of a screen into fsr_tmp[i] at the output size (integer rectangle), before the main pass */
static void fsr_upscale(int i, const Img *src, Rect *r) {
    if (!r->on) return;
    r->x = floorf(r->x + 0.5f); r->y = floorf(r->y + 0.5f); r->w = floorf(r->w + 0.5f); r->h = floorf(r->h + 0.5f);
    if (r->w < 1 || r->h < 1) { r->on = 0; return; }
    Img *t = &fsr_tmp[i];
    if (t->w != (uint32_t)r->w || t->h != (uint32_t)r->h) {
        img_free(t);
        if (img_make(t, (uint32_t)r->w, (uint32_t)r->h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) { r->on = 0; return; }
    }
    begin_pass(t, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    float ex[4] = { r->w, r->h, 0, 0 };
    gpu_quad(pipe_easu, src, (Rect){ 0, 0, r->w, r->h, 1 }, r->w, r->h, 0, 0, ex);
    vkCmdEndRendering(cmdbuf);
    barrier_img(t->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

/* a screen at the 3D internal resolution: un-rotate pica_vk's scan-out image (framebuffer orientation) into hi_scr[i] */
static const Img *hi_screen(int i, const ScreenSource *s, int screen_w) {
    VkImageView v; uint32_t lw, lh;
    if (!s->valid || (s->format & 7) != 1 || !pica_vk_scanout(s->addr, &v, &lw, &lh) || lw * (uint32_t)screen_w != lh * SCR_H) return NULL;
    Img *t = &hi_scr[i];
    if (t->w != lh || t->h != lw) {
        img_free(t);
        if (img_make(t, lh, lw, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return NULL;
    }
    begin_pass(t, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    gpu_quad_view(pipe_rgba, v, lw, lh, VK_IMAGE_LAYOUT_GENERAL, 1, (Rect){ 0, 0, (float)lh, (float)lw, 1 }, (float)lh, (float)lw, 0, 0, NULL);
    vkCmdEndRendering(cmdbuf);
    barrier_img(t->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    return t;
}

/* Samus Returns, R held (missiles): the HUD's beam slots from the bottom screen as it was before (msr_beams_held). Each
   frame R is not held, the slots' column (x 250-320, every row) is copied into beam_copy, at the size of the bottom
   screen in use; while it is held, the beam slots' quads sample that copy (the live screen if it has none). */
static Img beam_copy; static int beam_copy_ok;
static const Img *beam_slots(const Img *sb) {
    if (msr_beams_held()) return beam_copy_ok && beam_copy.w == sb->w && beam_copy.h == sb->h ? &beam_copy : sb;
    if (beam_copy.w != sb->w || beam_copy.h != sb->h) {
        img_free(&beam_copy); beam_copy_ok = 0;
        if (img_make(&beam_copy, sb->w, sb->h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { memset(&beam_copy, 0, sizeof beam_copy); return sb; }
    }
    const uint32_t x0 = sb->w * 250 / BOT_W;
    barrier_img(sb->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    barrier_img(beam_copy.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffset = { (int32_t)x0, 0, 0 },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffset = { (int32_t)x0, 0, 0 }, .extent = { sb->w - x0, sb->h, 1 } };
    vkCmdCopyImage(cmdbuf, sb->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, beam_copy.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    barrier_img(sb->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
    barrier_img(beam_copy.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    beam_copy_ok = 1;
    return sb;
}

static void upload(const Img *t, VkDeviceSize off);
/* a UI panel image of exactly w x h (remade when the UI scale changes; the previous frame has finished with it) */
static void ui_img(Img *t, int w, int h) {
    if (t->img && t->w == (uint32_t)w && t->h == (uint32_t)h) return;
    img_free(t);
    if (img_make(t, (uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) memset(t, 0, sizeof *t);
}
static void upload(const Img *t, VkDeviceSize off) {   /* whole image from up_buf + off */
    barrier_img(t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy cp = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { t->w, t->h, 1 } };
    vkCmdCopyBufferToImage(cmdbuf, up_buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier_img(t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static double now_s(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

/* The mouse in play (keyboard players): the left button shoots (3DS Y), the right one held is Free Aim (L) with the
   circle pad toward the pointer from Samus (msr_samus_view: where the game's camera shows her, then the zoom and the
   16:9 squeeze of msr_world_scale, then the top screen's place in the window). Over the top screen in play the pointer
   is a crosshair; it hides while a gamepad is in use and comes back when the mouse moves. */
static struct { float x, y, ax, ay; int fire, aim; double moved; } mouse = { 0, 0, 1, 0, 0, 0, 0 };
static void mouse_frame(Rect rt) {
    const int play = msr_world_in_play() && !msr_overlay_visible();
    if (!play) mouse.fire = mouse.aim = 0;
    float h, v, sx, sy;
    if (mouse.aim && msr_samus_view(&h, &v)) {
        msr_world_scale(&sx, &sy);   /* clip x is the screen's vertical (the 3DS's top screen is turned), y its horizontal */
        const float px = rt.x + rt.w * .5f * (1.f + h * sy), py = rt.y + rt.h * .5f * (1.f - v * sx);
        const float dx = mouse.x - px, dy = py - mouse.y, d = sqrtf(dx * dx + dy * dy);
        if (d > 3.f) { mouse.ax = dx / d; mouse.ay = dy / d; }   /* on Samus: the last direction */
    }
    input_mouse(mouse.fire, mouse.aim, mouse.ax, mouse.ay);
    static SDL_Cursor *cross, *arrow; static int shown = -1, crossed = -1;
    if (!cross) { cross = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR); arrow = SDL_GetDefaultCursor(); }
    const int over = mouse.x >= rt.x && mouse.x < rt.x + rt.w && mouse.y >= rt.y && mouse.y < rt.y + rt.h;
    const int want_cross = play && over, want_shown = !(play && msr_input_pad_active() && now_s() - mouse.moved > 2.0);
    if (cross && want_cross != crossed) { SDL_SetCursor(want_cross ? cross : arrow); crossed = want_cross; }
    if (want_shown != shown) { SDL_ShowCursor(want_shown ? SDL_ENABLE : SDL_DISABLE); shown = want_shown; }
}

int video_frame(void) {
    SDL_Event e; int bottom_click = 0;
    int ww, wh; SDL_GetWindowSize(win, &ww, &wh);
    Rect rt, rb; layout_rects(ww, wh, &rt, &rb);   /* window coordinates for touch; the drawing below uses drawable pixels */
    int dw0, dh0; SDL_Vulkan_GetDrawableSize(win, &dw0, &dh0);
    float msx = ww > 0 ? (float)dw0 / (float)ww : 1.f, msy = wh > 0 ? (float)dh0 / (float)wh : 1.f;   /* the overlay's mouse: drawable pixels */
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
#ifdef __ANDROID__
        if (e.type == SDL_APP_DIDENTERFOREGROUND) { surface_reset = 1; continue; }   /* a new window surface */
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && e.key.keysym.sym == SDLK_AC_BACK) {
            /* Back: the menu, as F1; in the launcher, out of what is open, as Escape */
            const int launcher = msr_overlay_launcher_mode();
            e.key.keysym.sym = launcher ? SDLK_ESCAPE : SDLK_F1;
            e.key.keysym.scancode = launcher ? SDL_SCANCODE_ESCAPE : SDL_SCANCODE_F1;
        }
#endif
        /* the settings overlay: F1 and Select (its binding) open and close it; while it shows, or waits for a key or button to
           bind, it takes the input */
        if (!msr_overlay_capturing()) {
            int key_toggle = (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && (e.key.keysym.sym == SDLK_F1 ||
                             (opt_select_layout == 2 && input_key_control(e.key.keysym.sym) == C_SELECT));
            int pad_toggle = (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) && !g_input_none &&
                             opt_select_layout == 2 && input_pad_control_now(e.cbutton.button) == C_SELECT;
            if (key_toggle || pad_toggle) {
                if ((e.type == SDL_KEYDOWN && !e.key.repeat) || e.type == SDL_CONTROLLERBUTTONDOWN) msr_overlay_toggle();
                continue;
            }
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN && !g_input_none) pad_used(pad_of(e.cbutton.which));   /* the settings name its buttons too */
        if (msr_overlay_event(&e, msx, msy)) continue;
        switch (e.type) {
        case SDL_QUIT: return 0;
        case SDL_CONTROLLERDEVICEADDED: case SDL_JOYDEVICEADDED: if (!g_input_none) pad_open(e.cdevice.which); break;
        case SDL_CONTROLLERDEVICEREMOVED: {
            SDL_GameController *c = pad_of(e.cdevice.which);
            if (!c) break;
            for (int i = 0; i < MAX_PADS; i++) if (gcs[i] == c) gcs[i] = NULL;
            SDL_GameControllerClose(c);
            if (gc == c) {
                gc = NULL; input_pad_stick(0, 0); input_pad_release();
                for (int i = 0; i < MAX_PADS; i++) if (gcs[i]) { pad_used(gcs[i]); break; }
                if (!gc) msr_input_device(0, 0);
            }
            break;
        }
        case SDL_JOYDEVICEREMOVED: if (joy && e.jdevice.which == SDL_JoystickInstanceID(joy)) { SDL_JoystickClose(joy); joy = NULL; input_pad_stick(0, 0); input_pad_release(); } break;
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: if (g_input_none) break;
            {   /* Samus Returns' touch navigation: L3 in menus; while on, the D-pad moves, the menus' choosing button is the
                   finger and their back button leaves (msr_pad_choose, msr_pad_back: the bottom and right ones, a Nintendo
                   pad's A and B; with the Nintendo menu layout, those bound to the game's A and B) */
                int c = e.cbutton.button, down = e.type == SDL_CONTROLLERBUTTONDOWN, choose = msr_pad_choose(), back = msr_pad_back();
                if (c == SDL_CONTROLLER_BUTTON_LEFTSTICK) { if (down) msr_touch_l3(); break; }
                if (msr_touch_active()) {
                    if (down && c == SDL_CONTROLLER_BUTTON_DPAD_UP) msr_touch_step(0, -1);
                    else if (down && c == SDL_CONTROLLER_BUTTON_DPAD_DOWN) msr_touch_step(0, 1);
                    else if (down && c == SDL_CONTROLLER_BUTTON_DPAD_LEFT) msr_touch_step(-1, 0);
                    else if (down && c == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) msr_touch_step(1, 0);
                    else if (c == choose) msr_touch_finger(down);
                    else if (down && c == back) msr_touch_close();
                    input_pad_sdl(c, down);   /* the pad's state stays true; the game sees none of it while blocked */
                    break;
                }
            }
            if (opt_select_layout == 1 && input_pad_control_now(e.cbutton.button) == C_SELECT) {
                if (e.type == SDL_CONTROLLERBUTTONDOWN) next_layout(); break; }
            {   /* ZL and ZR bound to a button: the triggers' taps */
                int c = input_pad_control_now(e.cbutton.button);
                if (c == C_ZL || c == C_ZR) { if (e.type == SDL_CONTROLLERBUTTONDOWN) msr_trigger(c == C_ZR); break; }
            }
            input_pad_sdl(e.cbutton.button, e.type == SDL_CONTROLLERBUTTONDOWN); break;
        case SDL_CONTROLLERAXISMOTION: if (!g_input_none) {
            {   /* a stick pushed past half way or a trigger pulled: this pad is in use */
                SDL_GameController *c = pad_of(e.caxis.which);
                if (c && c != gc && (e.caxis.value > 16000 || e.caxis.value < -16000)) pad_used(c);
                if (c && c != gc) break;   /* another pad's resting sticks are not read */
            }
            /* Samus Returns: LT = Morph Ball, RT = the next beam (taps on the HUD, msr_trigger), on the press */
            static int trig[2];
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                int t = e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT, on = e.caxis.value > 16000;
                if (on && !trig[t]) msr_trigger(t);
                trig[t] = on;
            }
            else pad_axes(); } break;
        case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: if (!joy || g_input_none || e.jbutton.button >= 8) break;
            if (opt_select_layout && e.jbutton.button == 6) { if (e.type == SDL_JOYBUTTONDOWN) { if (opt_select_layout == 2) msr_overlay_toggle(); else if (!msr_overlay_visible()) next_layout(); } break; }   /* raw pads: button 6 = Back/Select */
            if (!msr_overlay_visible()) input_pad_button(e.jbutton.button, e.type == SDL_JOYBUTTONDOWN); break;
        case SDL_JOYAXISMOTION: if (joy && !g_input_none) pad_axes(); break;
        case SDL_JOYHATMOTION: if (joy && !g_input_none) { input_pad_button(8, e.jhat.value & SDL_HAT_UP); input_pad_button(9, e.jhat.value & SDL_HAT_DOWN);
                                                           input_pad_button(10, e.jhat.value & SDL_HAT_LEFT); input_pad_button(11, e.jhat.value & SDL_HAT_RIGHT); } break;
        case SDL_KEYDOWN: case SDL_KEYUP:
            if (e.type == SDL_KEYDOWN && !e.key.repeat) msr_input_device(0, 0);   /* the keyboard names the buttons now */
            if (hotkey(&e.key)) break;
            {   /* Samus Returns' touch navigation: T toggles it (Tab is Start); while on, the circle pad's keys or the arrows
                   move, A's key or Enter is the finger, B's key or Escape leaves (the pad's layout: A on the right, B below) */
                SDL_Keycode c = e.key.keysym.sym; int down = e.type == SDL_KEYDOWN, ctl = input_key_control(c);
                if (c == SDLK_t && ctl < 0) { if (down && !e.key.repeat) msr_touch_toggle(); break; }
                if (msr_touch_active()) {
                    if (down && (c == SDLK_UP || ctl == C_CUP)) msr_touch_step(0, -1);
                    else if (down && (c == SDLK_DOWN || ctl == C_CDOWN)) msr_touch_step(0, 1);
                    else if (down && (c == SDLK_LEFT || ctl == C_CLEFT)) msr_touch_step(-1, 0);
                    else if (down && (c == SDLK_RIGHT || ctl == C_CRIGHT)) msr_touch_step(1, 0);
                    else if ((c == SDLK_RETURN || c == SDLK_KP_ENTER || ctl == C_A) && !e.key.repeat) msr_touch_finger(down);
                    else if (down && (c == SDLK_ESCAPE || ctl == C_B)) msr_touch_close();
                    if (!down) input_key(c, 0);   /* keys held when it opened still come up */
                    break;
                }
            }
            if (e.key.repeat) break;
            {   /* ZL and ZR (Left Shift and C): the triggers' Morph Ball and next beam */
                int c = input_key_control(e.key.keysym.sym);
                if (c == C_ZL || c == C_ZR) { if (e.type == SDL_KEYDOWN) msr_trigger(c == C_ZR); break; }
            }
            input_key(e.key.keysym.sym, e.type == SDL_KEYDOWN); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEMOTION: {
            if (e.type == SDL_MOUSEBUTTONDOWN) msr_input_device(0, 0);
            int down = e.type == SDL_MOUSEMOTION ? (e.motion.state & SDL_BUTTON_LMASK) != 0 : e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT;
            int mx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x, my = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
            int inside = rb.on && rb.w >= 1 && rb.h >= 1 && mx >= rb.x && mx < rb.x + rb.w && my >= rb.y && my < rb.y + rb.h;
            mouse.x = (float)mx; mouse.y = (float)my;
            if (e.type == SDL_MOUSEMOTION) mouse.moved = now_s();
            if (e.type != SDL_MOUSEMOTION) {   /* in play: the left button shoots (not on the bottom screen), the right one aims */
                int dn = e.type == SDL_MOUSEBUTTONDOWN, play = msr_world_in_play();
                if (e.button.button == SDL_BUTTON_RIGHT) { mouse.aim = dn && play; break; }
                if (e.button.button == SDL_BUTTON_LEFT && (mouse.fire || (dn && play && !inside))) { mouse.fire = dn; break; }
            }
            if (e.type == SDL_MOUSEMOTION && (!down || mouse.fire)) break;
            if (!rb.on || rb.w < 1 || rb.h < 1) { input_touch(0, -1, -1); break; }
            if (e.type == SDL_MOUSEBUTTONDOWN && inside) bottom_click = 1;
            input_touch(down, (int)((mx - rb.x) * BOT_W / rb.w), (int)((my - rb.y) * SCR_H / rb.h));
            break; }
        case SDL_MOUSEWHEEL: {   /* the wheel changes beams (down: the next, up: the one before), a notch at a time: each
                                    is a tap on the HUD's beam slots (msr_trigger), which needs ~200 ms to land */
            static float acc; static double last;
            msr_input_device(0, 0);
            float dy = e.wheel.preciseY;
            if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dy = -dy;
            acc += dy;
            if (fabsf(acc) < 1.f) break;
            const double t = now_s();
            if (t - last >= 0.2) { msr_trigger(acc < 0 ? 1 : 2); last = t; }
            acc = 0;
            break; }
        }
    }
    if (opt_layout == L_AUTO) auto_update(now_s(), bottom_click);
    mouse_frame(rt);
    {   /* touch navigation: the left stick steps or glides; the game's input comes back once nothing is held */
        float sx = 0, sy = 0;
        if (gc) { sx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f; sy = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f; }
        if (fabsf(sx) < .25f) sx = 0; if (fabsf(sy) < .25f) sy = 0;
        msr_touch_frame(sx, sy, input_buttons() != 0 || input_stick_active());
    }

    ScreenSource top, bottom;
    gsp_screens(&top, &bottom); decode_screen(&top, TOP_W, top_px);
    /* the bottom screen: from its scan-out copy's read-back when there is one (pica_vk.c: its guest copy is not made then) */
    if (!(gpu_ok && vk13 && bottom.valid && (bottom.format & 7) == 1 && pica_vk_bottom_pixels(bottom.addr, bot_px))) decode_screen(&bottom, BOT_W, bot_px);
    {   /* PWR_SEQ=<first presented frame>,<count>[,<every>]: both screens as decoded, one PPM pair per presented frame
           (or every <every>th of them) */
        static long first = -2, count, every = 1; static long n;
        if (first == -2) {
            const char *e = getenv("PWR_SEQ"); first = -1;
            if (e) { first = atol(e); const char *c = strchr(e, ','); count = c ? atol(c + 1) : 30; const char *s = c ? strchr(c + 1, ',') : NULL; if (s && atol(s + 1) > 1) every = atol(s + 1); }
        }
        if (first >= 0 && n >= first && n < first + count && (n - first) % every == 0) {
            { extern void k_ev(int, uint32_t, uint32_t); k_ev(0x30, (uint32_t)(n - first), bottom.addr); }
            char fn[64]; snprintf(fn, sizeof fn, "seq_%04ld_b_%08x.ppm", n - first, bottom.addr); FILE *f = fopen(fn, "wb");
            if (f) { fprintf(f, "P6 %d %d 255\n", BOT_W, SCR_H); for (int i = 0; i < BOT_W * SCR_H; i++) fwrite(bot_px + 4 * i, 1, 3, f); fclose(f); }
            snprintf(fn, sizeof fn, "seq_%04ld_t_%08x.ppm", n - first, top.addr); f = fopen(fn, "wb");
            if (f) { fprintf(f, "P6 %d %d 255\n", TOP_W, SCR_H); for (int i = 0; i < TOP_W * SCR_H; i++) fwrite(top_px + 4 * i, 1, 3, f); fclose(f); }
        }
        n++;
    }
    static int test = -1; if (test < 0) test = getenv("PWR_TESTPATTERN") != NULL;
    if (test) for (int y = 0; y < SCR_H; y++) for (int x = 0; x < 50; x++) {   /* colour bars down the left edge of both screens */
        int band = y * 6 / SCR_H; uint8_t *d = top_px + ((size_t)y * TOP_W + (size_t)x) * 4, *d2 = bot_px + ((size_t)y * BOT_W + (size_t)x) * 4;
        d[0] = d2[0] = (band & 1) ? 255 : 0; d[1] = d2[1] = (band & 2) ? 255 : 0; d[2] = d2[2] = (band & 4) ? 255 : 0;
    }

    /* window resized or swapchain lost: rebuild before drawing */
    static int last_dw = -1, last_dh = -1; int dw, dh; SDL_Vulkan_GetDrawableSize(win, &dw, &dh);
    if (last_dw >= 0 && (dw != last_dw || dh != last_dh)) { if (recreate_swapchain()) return 0; if (opt_res_match) match_resolution(dw, dh); }
    last_dw = dw; last_dh = dh;
    msr_overlay_frame();   /* the settings overlay's actions: before an image is acquired, as V-Sync rebuilds the swapchain */
    {   /* save states: "Saved", "Loaded"... where the layout label goes */
        const char *m = msr_state_message();
        if (m) { static char shown[64]; snprintf(shown, sizeof shown, "%s", m); toast_text = shown; toast_until = now_s() + 2.0; toast_dirty = 1; }
    }
    {   /* HDR: the display's state when the settings open (Windows' "Use HDR" may have changed), the player's setting */
        static int was_visible; int vis = msr_overlay_visible();
        if (vis && !was_visible) hdr_detect();
        was_visible = vis;
        if (swap && gpu_ok && hdr_wanted() != (hdr_mode != 0)) { if (recreate_swapchain()) return 0; }
    }
    /* minimised, the swapchain waits at size 0 (create_swapchain). SDL keeps the window's size through a minimise, so
       the size check above never sees the restore: try again once the window is back, or the picture froze for good */
    if (swap && (!sc_extent.width || !sc_extent.height) && !(SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED)) { if (recreate_swapchain()) return 0; }
    if (surface_reset) { surface_reset = 0; if (recreate_surface()) return 0; }
    if (!swap || !sc_extent.width || !sc_extent.height) return 1;

    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vk_push_reset(&push_arena);   /* the last frame's sets are done with */
    uint32_t idx;
    VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, sem_acquire, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { g_vid_acquire_fail++; g_vid_last_err = r; }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return recreate_swapchain() ? 0 : 1;
    if (r == VK_ERROR_SURFACE_LOST_KHR) { surface_reset = 1; return 1; }
    vkResetFences(dev, 1, &fence);
    layout_rects((int)sc_extent.width, (int)sc_extent.height, &rt, &rb);   /* drawable pixels (differs from window size on HiDPI) */
    /* Samus Returns' single screen: the compositor places the view (the gallery moves it aside) and lists the HUD
       pieces, menus and prompts drawn over it; the bottom screen is never drawn as a screen */
    int single = effective_layout() == L_SINGLE; MsrFrame mf = { { 0 }, { 0 }, NULL, 0 };
    msr_set_single(single);
    msr_compose(bot_px, now_s(), input_buttons(), (float)sc_extent.width, (float)sc_extent.height, &mf);   /* classifies in every layout (the triggers' taps need it) */
    if (single) { rt = (Rect){ mf.top[0], mf.top[1], mf.top[2], mf.top[3], 1 }; rb.on = 0; }
    else mf.count = 0;
    msr_overlay_panel(single ? mf.panel : NULL);   /* the control hints go beside the menus' panel */

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkResetCommandBuffer(cmdbuf, 0); vkBeginCommandBuffer(cmdbuf, &bi);
    if (gpu_ok && vk13) pica_vk_scan_lock(1);   /* until submitted: a 3D resolution change must not free a scan-out image in use */
    if (gpu_ok) {
        float W = (float)sc_extent.width, H = (float)sc_extent.height;
        memcpy(up_ptr, top_px, sizeof top_px); memcpy(up_ptr + sizeof top_px, bot_px, sizeof bot_px);
        upload(&tex_top, 0); upload(&tex_bot, sizeof top_px);
        /* the layout label's scale: by window height */
        int ks = (int)(H / 480.0f); ks = ks < 1 ? 1 : ks > UI_KMAX ? UI_KMAX : ks;
        /* screens at the 3D internal resolution when pica_vk kept them (its scan-out images); the scan-out copies were submitted earlier */
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
        vkCmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        const Img *st = hi_screen(0, &top, TOP_W), *sb = hi_screen(1, &bottom, BOT_W);
        if (!st) st = &tex_top; if (!sb) sb = &tex_bot;
        const Img *sbeams = single ? beam_slots(sb) : sb;   /* R held: the beam slots before it (outside the passes) */
        if (toast_text && (toast_dirty || tex_toast.w != (uint32_t)(TOAST_W * ks))) {
            int w = TOAST_W * ks, h = TOAST_H * ks; ui_img(&tex_toast, w, h);
            fill(toast_px, w, 0, 0, w, h, 0x101820, 210);
            text(toast_px, w, h, (w - 8 * ks * (int)strlen(toast_text)) / 2, 3 * ks, toast_text, 0xffffff, ks);
            memcpy(up_ptr + UP_TOAST, toast_px, (size_t)w * h * 4); upload(&tex_toast, UP_TOAST); toast_dirty = 0;
        }
        int fsr = opt_filter == F_FSR;
        if (fsr) { fsr_upscale(0, st, &rt); fsr_upscale(1, sb, &rb); }
        begin_pass(&target, VK_ATTACHMENT_LOAD_OP_CLEAR);
        if (fsr) {
            float et[4] = { opt_sharp, 0, rt.x, rt.y }, eb[4] = { opt_sharp, 0, rb.x, rb.y };
            if (rt.on) gpu_quad(pipe_rcas, &fsr_tmp[0], rt, W, H, 0, 0, et);
            if (rb.on) gpu_quad(pipe_rcas, &fsr_tmp[1], rb, W, H, 0, 0, eb);
        } else {
            int f = opt_filter == F_NEAREST ? 0 : opt_filter == F_SHARP ? 2 : opt_filter == F_FXAA ? 4 : 1;
            gpu_quad(pipe, st, rt, W, H, f, 0, NULL); gpu_quad(pipe, sb, rb, W, H, f, 0, NULL);
        }
        if (hdr_mode) {   /* HDR: the interface goes into its own layer, at paper white in hdr.frag */
            vkCmdEndRendering(cmdbuf);
            barrier_img(target.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            begin_pass_a(&ui_layer, VK_ATTACHMENT_LOAD_OP_CLEAR, 0);
        }
        if (mf.count) {
            vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, hdr_mode ? pipe_msr_ui : pipe_msr);
            for (int i = 0; i < mf.count; i++) msr_quad(&mf.quads[i], mf.quads[i].source ? sbeams : sb, W, H);
        }
        if (!msr_overlay_visible() && now_s() < toast_until && tex_toast.img)   /* layout label, top centre (pixel for pixel) */
            gpu_quad(hdr_mode ? pipe_ui : pipe, &tex_toast, (Rect){ floorf((W - (float)tex_toast.w) / 2), 12, (float)tex_toast.w, (float)tex_toast.h, 1 }, W, H, 0, 1, NULL);
        msr_overlay_draw(cmdbuf, sc_extent.width, sc_extent.height);   /* the settings, the FPS counter, the hints: last */
        vkCmdEndRendering(cmdbuf);
        if (hdr_mode) {
            barrier_img(ui_layer.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = sc_views[idx], .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
            VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, sc_extent }, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
            vkCmdBeginRendering(cmdbuf, &ri);
            VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, sc_extent };
            vkCmdSetViewport(cmdbuf, 0, 1, &vp); vkCmdSetScissor(cmdbuf, 0, 1, &sc);
            float pk, pp; msr_hdr(&pk, &pp); if (pk <= 0) pk = disp_peak; if (pp <= 0) pp = disp_paper;
            Push hp = { { -1, -1, 1, 1 }, { W, H, 1, 1 }, { hdr_mode, 0, 0, 0 }, { pp, pk > pp ? pk : pp, 0.5f, 0 } };
            VkDescriptorImageInfo ii[2] = { { smp_linear, target.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { smp_linear, ui_layer.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
            VkWriteDescriptorSet wds[2] = { { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[0] },
                                            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[1] } };
            vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_hdr);
            vk_push(&push_arena, cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, play_hdr, 0, 2, wds);
            vkCmdPushConstants(cmdbuf, play_hdr, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof hp, &hp);
            vkCmdDraw(cmdbuf, 4, 1, 0, 0);
            vkCmdEndRendering(cmdbuf);
            barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        } else {
            barrier_img(target.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                               .extent = { sc_extent.width, sc_extent.height, 1 } };
            vkCmdCopyImage(cmdbuf, target.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        }
    } else {
        memset(stage_ptr, 0, (size_t)stage_size);
        cpu_blit(top_px, TOP_W, rt); cpu_blit(bot_px, BOT_W, rb);
        barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy cp = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { sc_extent.width, sc_extent.height, 1 } };
        vkCmdCopyBufferToImage(cmdbuf, stage, sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    }
    barrier_img(sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cmdbuf);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_acquire, .pWaitDstStageMask = &ws,
        .commandBufferCount = 1, .pCommandBuffers = &cmdbuf, .signalSemaphoreCount = 1, .pSignalSemaphores = &sem_done[idx] };
    pthread_mutex_lock(&queue_lock);
    vkQueueSubmit(queue, 1, &sub, fence);
    if (gpu_ok && vk13) pica_vk_scan_lock(0);
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sem_done[idx], .swapchainCount = 1, .pSwapchains = &swap, .pImageIndices = &idx };
    r = vkQueuePresentKHR(queue, &pi);
    pthread_mutex_unlock(&queue_lock);
#ifdef __ANDROID__
    if (r == VK_SUBOPTIMAL_KHR) r = VK_SUCCESS;   /* not pre-rotated (create_swapchain: the compositor turns it), every frame */
#endif
    if (r != VK_SUCCESS) { g_vid_present_fail++; g_vid_last_err = r; } else g_vid_presented++;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
    return 1;
}

int video_vk(VkPhysicalDevice *p, VkDevice *d, VkQueue *q, uint32_t *family) {
    if (!dev || !vk13) return -1;
    *p = phys; *d = dev; *q = queue; *family = qfam; return 0;
}
void video_queue_lock(void) { pthread_mutex_lock(&queue_lock); }
void video_queue_unlock(void) { pthread_mutex_unlock(&queue_lock); }

void video_shutdown(void) {
    if (dev) { pthread_mutex_lock(&queue_lock); vkDeviceWaitIdle(dev); pthread_mutex_unlock(&queue_lock); if (gpu_ok) msr_overlay_shutdown(); }
    SDL_Quit();
}
