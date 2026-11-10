/* Render passes and framebuffers for the Vulkan 1.1 mode (vk_rp.h). Both are cached: render passes by their formats,
 * layout and ops (a handful), framebuffers by their pass, views and size, until a view they use is destroyed
 * (vk_rp_forget). The renderer's thread and the presenter's share the caches under one lock. */
#include "vk_rp.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_vk_rp;

static VkDevice dev;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct { VkFormat color, depth; VkImageLayout layout; VkAttachmentLoadOp cload, dload; VkAttachmentStoreOp dstore; int stencil; VkRenderPass rp; } Rp;
#define MAX_RP 32
static Rp rps[MAX_RP]; static int nrp;

typedef struct { VkRenderPass rp; VkImageView c, d; uint32_t w, h; VkFramebuffer fb; } Fb;
static Fb *fbs; static int nfb, capfb, last_fb = -1;

void vk_rp_init(VkDevice d) { dev = d; }

VkRenderPass vk_rp_get(VkFormat color, VkImageLayout layout, VkAttachmentLoadOp cload, VkFormat depth, int stencil,
                       VkAttachmentLoadOp dload, VkAttachmentStoreOp dstore) {
    if (depth == VK_FORMAT_UNDEFINED) { stencil = 0; dload = VK_ATTACHMENT_LOAD_OP_DONT_CARE; dstore = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nrp; i++) {
        const Rp *r = &rps[i];
        if (r->color == color && r->layout == layout && r->cload == cload && r->depth == depth && r->stencil == stencil &&
            r->dload == dload && r->dstore == dstore) { VkRenderPass rp = r->rp; pthread_mutex_unlock(&lock); return rp; }
    }
    VkAttachmentDescription at[2] = {
        { .format = color, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = cload, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = layout, .finalLayout = layout },
        { .format = depth, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = dload, .storeOp = dstore,
          .stencilLoadOp = stencil ? dload : VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = stencil ? dstore : VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL } };
    VkAttachmentReference cr = { 0, layout }, dr = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &cr,
        .pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &dr : NULL };
    /* the passes' callers order them with pipeline barriers recorded outside the pass (as with dynamic rendering) */
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = depth != VK_FORMAT_UNDEFINED ? 2 : 1,
        .pAttachments = at, .subpassCount = 1, .pSubpasses = &sp };
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev, &ci, NULL, &rp)) { fprintf(stderr, "video: render pass creation failed\n"); rp = VK_NULL_HANDLE; }
    else if (nrp < MAX_RP) rps[nrp++] = (Rp){ color, depth, layout, cload, dload, dstore, stencil, rp };
    else fprintf(stderr, "video: too many render passes\n");
    pthread_mutex_unlock(&lock);
    return rp;
}

static VkFramebuffer fb_get(VkRenderPass rp, VkImageView c, VkImageView d, uint32_t w, uint32_t h) {
    if (last_fb >= 0 && last_fb < nfb) {
        const Fb *f = &fbs[last_fb];
        if (f->rp == rp && f->c == c && f->d == d && f->w == w && f->h == h) return f->fb;
    }
    for (int i = 0; i < nfb; i++) {
        const Fb *f = &fbs[i];
        if (f->rp == rp && f->c == c && f->d == d && f->w == w && f->h == h) { last_fb = i; return f->fb; }
    }
    VkImageView v[2] = { c, d };
    VkFramebufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = d ? 2 : 1,
        .pAttachments = v, .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(dev, &ci, NULL, &fb)) { fprintf(stderr, "video: framebuffer creation failed (%ux%u)\n", w, h); return VK_NULL_HANDLE; }
    if (nfb == capfb) { int cap = capfb ? capfb * 2 : 64; Fb *n = realloc(fbs, (size_t)cap * sizeof *n); if (!n) { vkDestroyFramebuffer(dev, fb, NULL); return VK_NULL_HANDLE; } fbs = n; capfb = cap; }
    fbs[nfb] = (Fb){ rp, c, d, w, h, fb }; last_fb = nfb++;
    return fb;
}

void vk_rp_begin(VkCommandBuffer cb, VkRenderPass rp, VkImageView c, VkImageView d, VkExtent2D size, const VkClearValue *clear) {
    pthread_mutex_lock(&lock);
    VkFramebuffer fb = fb_get(rp, c, d, size.width, size.height);
    pthread_mutex_unlock(&lock);
    if (!fb) return;   /* (the caller's draws and end then fail validation; the log says why) */
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = { { 0, 0 }, size }, .clearValueCount = clear ? 1 : 0, .pClearValues = clear };
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

void vk_rp_forget(VkImageView view) {
    if (!view) return;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < nfb;) {
        if (fbs[i].c == view || fbs[i].d == view) { vkDestroyFramebuffer(dev, fbs[i].fb, NULL); fbs[i] = fbs[--nfb]; }
        else i++;
    }
    last_fb = -1;
    pthread_mutex_unlock(&lock);
}
