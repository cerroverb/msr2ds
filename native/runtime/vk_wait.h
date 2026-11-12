/* Waits on the GPU that never give up early by mistake (video.c, pica_vk.c). vkWaitForFences and vkQueueWaitIdle are
 * tried again after VK_TIMEOUT, and after VK_ERROR_INITIALIZATION_FAILED, which Mali's driver returns when a signal
 * interrupts its wait (its ppoll is not restarted: Azahar retries the same way, vk_present_window.cpp). Before, every
 * result was ignored, and a wait that ended early let the caller reset a fence still pending and reuse descriptor sets,
 * command buffers and ring space the GPU was still reading (#52: a Mali-G57 whose screen stayed black, the main thread
 * stuck in a wait for good). Any other result (the device lost) is logged once and returned: the caller must not reuse
 * what the wait guards. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan.h>
void k_sleep_ns(int64_t ns);   /* kernel.c */
#define VK_WAIT_NAP() k_sleep_ns(1000000)

static inline VkResult vk_wait_retry(VkResult r, unsigned *tries, const char *what) {
    static int told_retry, told_fail;
    if (r == VK_TIMEOUT || r == VK_ERROR_INITIALIZATION_FAILED) {
        if (!told_retry) { told_retry = 1; fprintf(stderr, "video: %s returned %d: waiting again (Mali drivers do this)\n", what, (int)r); }
        if (++*tries > 1000) VK_WAIT_NAP();   /* (not returning at all would spin: a millisecond between tries) */
        return VK_INCOMPLETE;
    }
    if (r != VK_SUCCESS && told_fail < 5) { told_fail++; fprintf(stderr, "video: %s failed (%d)\n", what, (int)r); }
    return r;
}
static inline VkResult vk_wait_fences(VkDevice dev, uint32_t n, const VkFence *f) {
    unsigned tries = 0; VkResult r;
    while ((r = vk_wait_retry(vkWaitForFences(dev, n, f, VK_TRUE, UINT64_MAX), &tries, "vkWaitForFences")) == VK_INCOMPLETE) {}
    return r;
}
static inline VkResult vk_wait_queue(VkQueue q) {
    unsigned tries = 0; VkResult r;
    while ((r = vk_wait_retry(vkQueueWaitIdle(q), &tries, "vkQueueWaitIdle")) == VK_INCOMPLETE) {}
    return r;
}
static inline VkResult vk_wait_device(VkDevice dev) {
    unsigned tries = 0; VkResult r;
    while ((r = vk_wait_retry(vkDeviceWaitIdle(dev), &tries, "vkDeviceWaitIdle")) == VK_INCOMPLETE) {}
    return r;
}
