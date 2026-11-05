/* Push descriptors (VK_KHR_push_descriptor), or descriptor sets standing in for them where the device has none (some
 * phones' GPUs, the Android emulator): vk_push.c. The renderer (pica_vk.c) and the presenter (video.c) push their
 * descriptors through vk_push; a set layout pushed to is made with vk_push_layout_flags() and named with
 * vk_push_layout; an arena holds the stand-in sets of one submission and is reset once its fence has signalled. */
#pragma once
#include <vulkan/vulkan.h>

typedef struct { VkDescriptorPool *pools; int n, cap, cur; } VkPushArena;

void vk_push_init(VkDevice dev, int supported);   /* video.c, once the device exists (supported: the extension's on) */
int vk_push_native(void);                         /* the device's own push descriptors */
VkDescriptorSetLayoutCreateFlags vk_push_layout_flags(void);
void vk_push_layout(VkPipelineLayout layout, VkDescriptorSetLayout set0);   /* the set layout a pipeline layout pushes */
void vk_push(VkPushArena *arena, VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipelineLayout layout, uint32_t set,
             uint32_t count, const VkWriteDescriptorSet *writes);
void vk_push_reset(VkPushArena *arena);           /* after the fence of the submission that used it */
