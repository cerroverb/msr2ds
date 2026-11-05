/* The Vulkan 1.3 functions the renderer calls (dynamic rendering, extended dynamic state), for the Android app: the
 * NDK's libvulkan.so exports only Vulkan 1.1's, so these are the device's own, looked up once it exists
 * (msr_vk13_load from video.c; the KHR and EXT names of the extensions they came from, where the core name is not
 * there). */
#include <stdio.h>
#include <vulkan/vulkan.h>

void msr_vk13_load(VkDevice dev);

#define MSR_VK13(X)                                                                                                    \
    X(vkCmdBeginRendering, "vkCmdBeginRenderingKHR", (VkCommandBuffer cb, const VkRenderingInfo *info), (cb, info))   \
    X(vkCmdEndRendering, "vkCmdEndRenderingKHR", (VkCommandBuffer cb), (cb))                                         \
    X(vkCmdSetCullMode, "vkCmdSetCullModeEXT", (VkCommandBuffer cb, VkCullModeFlags mode), (cb, mode))               \
    X(vkCmdSetFrontFace, "vkCmdSetFrontFaceEXT", (VkCommandBuffer cb, VkFrontFace face), (cb, face))                 \
    X(vkCmdSetDepthTestEnable, "vkCmdSetDepthTestEnableEXT", (VkCommandBuffer cb, VkBool32 on), (cb, on))            \
    X(vkCmdSetDepthWriteEnable, "vkCmdSetDepthWriteEnableEXT", (VkCommandBuffer cb, VkBool32 on), (cb, on))          \
    X(vkCmdSetDepthCompareOp, "vkCmdSetDepthCompareOpEXT", (VkCommandBuffer cb, VkCompareOp op), (cb, op))           \
    X(vkCmdSetStencilTestEnable, "vkCmdSetStencilTestEnableEXT", (VkCommandBuffer cb, VkBool32 on), (cb, on))        \
    X(vkCmdSetStencilOp, "vkCmdSetStencilOpEXT",                                                                     \
      (VkCommandBuffer cb, VkStencilFaceFlags faces, VkStencilOp fail, VkStencilOp pass, VkStencilOp depth_fail,      \
       VkCompareOp op),                                                                                                \
      (cb, faces, fail, pass, depth_fail, op))

#define MSR_VK13_DEFINE(name, alt, params, args)                                                                       \
    static PFN_##name p_##name;                                                                                        \
    VKAPI_ATTR void VKAPI_CALL name params { p_##name args; }
MSR_VK13(MSR_VK13_DEFINE)

void msr_vk13_load(VkDevice dev) {
#define MSR_VK13_LOAD(name, alt, params, args)                                                                         \
    p_##name = (PFN_##name)vkGetDeviceProcAddr(dev, #name);                                                            \
    if (!p_##name) p_##name = (PFN_##name)vkGetDeviceProcAddr(dev, alt);                                               \
    if (!p_##name) fprintf(stderr, "video: the device has no %s\n", #name);
    MSR_VK13(MSR_VK13_LOAD)
}
