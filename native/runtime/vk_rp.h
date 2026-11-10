/* The Vulkan 1.1 mode (g_vk_rp, set by video.c where the device has no Vulkan 1.3 dynamic rendering: phones' GPUs of
 * about 2019-2021, Adreno 6xx, Mali-G7x): render passes and framebuffers stand in for dynamic rendering (vk_rp.c). The
 * renderer (pica_vk.c) and the presenter (video.c) begin their passes through vk_rp_begin; a pipeline is made for
 * vk_rp_get's pass of the same formats (render passes that differ only in load and store ops are compatible). */
#pragma once
#include <vulkan/vulkan.h>

extern int g_vk_rp;   /* video.c: 1 = the Vulkan 1.1 mode */

void vk_rp_init(VkDevice dev);
/* a render pass with one colour attachment, kept in `layout` (the layout it is in before and after), and, when `depth`
   is not VK_FORMAT_UNDEFINED, a depth (and stencil) attachment kept in GENERAL */
VkRenderPass vk_rp_get(VkFormat color, VkImageLayout layout, VkAttachmentLoadOp cload, VkFormat depth, int stencil,
                       VkAttachmentLoadOp dload, VkAttachmentStoreOp dstore);
/* begins rp on these views (d: VK_NULL_HANDLE without depth), its framebuffer made the first time; clear: the colour
   attachment's clear value (a CLEAR load op), or NULL */
void vk_rp_begin(VkCommandBuffer cb, VkRenderPass rp, VkImageView c, VkImageView d, VkExtent2D size, const VkClearValue *clear);
/* before a view is destroyed (its last use finished): the framebuffers made with it go too */
void vk_rp_forget(VkImageView view);
