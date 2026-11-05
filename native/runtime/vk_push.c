/* Push descriptors, or descriptor sets standing in for them (vk_push.h). Without VK_KHR_push_descriptor each push is a
 * descriptor set from the arena's pools, written and bound; the arena's pools are reset when its submission is done and
 * kept for the next, a new pool added when they are full. */
#include "vk_push.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static VkDevice dev;
static int native;
static PFN_vkCmdPushDescriptorSetKHR push_desc;
#define MAX_LAYOUTS 16
static struct { VkPipelineLayout layout; VkDescriptorSetLayout set0; } layouts[MAX_LAYOUTS];
static int nlayouts;
static pthread_mutex_t layouts_lock = PTHREAD_MUTEX_INITIALIZER;

void vk_push_init(VkDevice d, int supported) {
    dev = d;
    push_desc = supported ? (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(d, "vkCmdPushDescriptorSetKHR") : NULL;
    native = push_desc != NULL;
    if (!native) fprintf(stderr, "video: no push descriptors: descriptor sets stand in for them\n");
}

int vk_push_native(void) { return native; }

VkDescriptorSetLayoutCreateFlags vk_push_layout_flags(void) {
    return native ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
}

void vk_push_layout(VkPipelineLayout layout, VkDescriptorSetLayout set0) {
    pthread_mutex_lock(&layouts_lock);
    if (nlayouts < MAX_LAYOUTS) { layouts[nlayouts].layout = layout; layouts[nlayouts].set0 = set0; nlayouts++; }
    else fprintf(stderr, "video: too many push descriptor layouts\n");
    pthread_mutex_unlock(&layouts_lock);
}

static VkDescriptorSetLayout set_layout(VkPipelineLayout layout) {
    VkDescriptorSetLayout s = VK_NULL_HANDLE;
    pthread_mutex_lock(&layouts_lock);
    for (int i = 0; i < nlayouts; i++) if (layouts[i].layout == layout) { s = layouts[i].set0; break; }
    pthread_mutex_unlock(&layouts_lock);
    return s;
}

static VkDescriptorPool new_pool(void) {
    /* 256 sets of the renderer's largest (2 uniform buffers, 3 storage buffers, 3 images) */
    const VkDescriptorPoolSize sizes[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512 },
                                            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 768 },
                                            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 768 } };
    const VkDescriptorPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 256,
                                            .poolSizeCount = 3, .pPoolSizes = sizes };
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(dev, &ci, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

void vk_push(VkPushArena *a, VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipelineLayout layout, uint32_t set,
             uint32_t count, const VkWriteDescriptorSet *writes) {
    if (native) { push_desc(cb, bind, layout, set, count, writes); return; }
    const VkDescriptorSetLayout sl = set_layout(layout);
    if (!sl || count > 16) { fprintf(stderr, "video: a push to an unknown layout\n"); return; }
    VkDescriptorSet ds = VK_NULL_HANDLE;
    for (;;) {
        if (a->cur == a->n) {   /* every pool full: another */
            if (a->n == a->cap) {
                const int cap = a->cap ? a->cap * 2 : 4;
                VkDescriptorPool *pools = realloc(a->pools, (size_t)cap * sizeof *pools);
                if (!pools) return;
                a->pools = pools; a->cap = cap;
            }
            if (!(a->pools[a->n] = new_pool())) { fprintf(stderr, "video: no descriptor pool\n"); return; }
            a->n++;
        }
        const VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = a->pools[a->cur], .descriptorSetCount = 1, .pSetLayouts = &sl };
        if (vkAllocateDescriptorSets(dev, &ai, &ds) == VK_SUCCESS) break;
        a->cur++;   /* this pool is full: the next */
    }
    VkWriteDescriptorSet w[16];
    for (uint32_t i = 0; i < count; i++) { w[i] = writes[i]; w[i].dstSet = ds; }
    vkUpdateDescriptorSets(dev, count, w, 0, NULL);
    vkCmdBindDescriptorSets(cb, bind, layout, set, 1, &ds, 0, NULL);
}

void vk_push_reset(VkPushArena *a) {
    if (native) return;
    for (int i = 0; i < a->n; i++) vkResetDescriptorPool(dev, a->pools[i], 0);
    a->cur = 0;
}
