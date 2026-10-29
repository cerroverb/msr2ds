// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Seam between the Vulkan renderer and the Samus Returns settings overlay. The renderer only
// knows this interface; the frontend registers the ImGui implementation (msr/overlay.h), so
// video_core gains no ImGui dependency and plain builds never see an overlay.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vulkan/vulkan.h>

namespace MSR::OverlayHook {

struct VulkanContext {
    std::uint32_t api_version;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    std::uint32_t queue_family;
    VkQueue queue;
    VkRenderPass render_pass; ///< The presentation render pass the overlay draws inside
    std::uint32_t image_count;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    std::mutex* queue_mutex; ///< Held while the overlay submits its own uploads
};

class Interface {
public:
    virtual ~Interface() = default;
    /// The renderer was created (each boot); create GPU resources.
    virtual void Init(const VulkanContext& context) = 0;
    /// Records the overlay into `command`, inside the presentation render pass.
    virtual void Draw(VkCommandBuffer command, std::uint32_t width, std::uint32_t height) = 0;
    /// The renderer is going away and the device is idle; release GPU resources.
    virtual void Shutdown() = 0;
};

inline std::atomic<Interface*> overlay{nullptr};

} // namespace MSR::OverlayHook
