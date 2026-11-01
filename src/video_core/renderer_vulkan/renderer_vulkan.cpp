// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <filesystem>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/memory_detect.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_memory_util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

// After the renderer headers, which configure the Vulkan headers.
#include "msr/bottom_classifier.h"
#include "msr/draw_probe.h"
#include "msr/overlay_hook.h"
#include "msr/presentation.h"

#include "video_core/host_shaders/vulkan_present_anaglyph_frag.h"
#include "video_core/host_shaders/vulkan_present_frag.h"
#include "video_core/host_shaders/vulkan_present_interlaced_frag.h"
#include "video_core/host_shaders/vulkan_present_msr_frag.h"
#include "video_core/host_shaders/vulkan_present_vert.h"

#include "video_core/host_shaders/vulkan_cursor_frag.h"
#include "video_core/host_shaders/vulkan_cursor_vert.h"

#include <vk_mem_alloc.h>
#if defined(__APPLE__) && !defined(HAVE_LIBRETRO)
#include "common/apple_utils.h"
#endif

#ifdef ENABLE_SDL2
#include <SDL.h>
#endif

MICROPROFILE_DEFINE(Vulkan_RenderFrame, "Vulkan", "Render Frame", MP_RGB(128, 128, 64));

namespace Vulkan {

struct ScreenRectVertex {
    ScreenRectVertex() = default;
    ScreenRectVertex(float x, float y, float u, float v)
        : position{Common::MakeVec(x, y)}, tex_coord{Common::MakeVec(u, v)} {}

    Common::Vec2f position;
    Common::Vec2f tex_coord;
};

constexpr u32 VERTEX_BUFFER_SIZE = sizeof(ScreenRectVertex) * 8192;

constexpr std::array<f32, 4 * 4> MakeOrthographicMatrix(u32 width, u32 height) {
    // clang-format off
    return { 2.f / width, 0.f,         0.f, -1.f,
            0.f,         2.f / height, 0.f, -1.f,
            0.f,         0.f,          1.f,  0.f,
            0.f,         0.f,          0.f,  1.f};
    // clang-format on
}

constexpr static std::array<vk::DescriptorSetLayoutBinding, 1> PRESENT_BINDINGS = {{
    {0, vk::DescriptorType::eCombinedImageSampler, 3, vk::ShaderStageFlagBits::eFragment},
}};

namespace {
static bool IsLowRefreshRate() {
#if (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)
    if (!Settings::values.use_display_refresh_rate_detection) {
        LOG_INFO(Render_Vulkan, "Refresh rate detection is currently disabled via settings");
        return false;
    }
#ifdef __APPLE__
    // Apple's low power mode sometimes limits applications to 30fps without changing the refresh
    // rate, meaning the above code doesn't catch it.
    if (AppleUtils::IsLowPowerModeEnabled()) {
        LOG_WARNING(Render_Vulkan, "Apple's low power mode is enabled, assuming low application "
                                   "framerate. FIFO will be disabled");
        return true;
    }

    const auto cur_refresh_rate = AppleUtils::GetRefreshRate();
#elif defined(ENABLE_SDL2)
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        LOG_ERROR(Render_Vulkan, "Attempted to check refresh rate via SDL, but failed because "
                                 "SDL_INIT_VIDEO wasn't initialized");
        return false;
    }

    SDL_DisplayMode cur_display_mode;
    SDL_GetCurrentDisplayMode(0, &cur_display_mode); // TODO: Multimonitor handling. -OS

    const auto cur_refresh_rate = cur_display_mode.refresh_rate;
#endif // ENABLE_SDL2

    if (cur_refresh_rate < SCREEN_REFRESH_RATE) {
        LOG_WARNING(Render_Vulkan,
                    "Detected refresh rate lower than the emulated 3DS screen: {}hz. FIFO will "
                    "be disabled",
                    cur_refresh_rate);
        return true;
    } else {
        LOG_INFO(Render_Vulkan, "Refresh rate is above emulated 3DS screen: {}hz. Good.",
                 cur_refresh_rate);
    }
#endif // (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)

    // We have no available method of checking refresh rate. Just assume that everything is fine :)
    return false;
}
} // Anonymous namespace

RendererVulkan::RendererVulkan(Core::System& system, Pica::PicaCore& pica_,
                               Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window)
    : RendererBase{system, window, secondary_window}, memory{system.Memory()}, pica{pica_},
      instance{window, Settings::values.physical_device.GetValue()}, scheduler{instance},
      renderpass_cache{instance, scheduler},
      main_present_window{window, instance, scheduler, IsLowRefreshRate()},
      vertex_buffer{instance, scheduler, vk::BufferUsageFlagBits::eVertexBuffer,
                    VERTEX_BUFFER_SIZE},
      update_queue{instance}, rasterizer{memory,
                                         pica,
                                         system.CustomTexManager(),
                                         *this,
                                         render_window,
                                         instance,
                                         scheduler,
                                         renderpass_cache,
                                         update_queue,
                                         main_present_window.ImageCount()},
      present_heap{instance, scheduler.GetMasterSemaphore(), PRESENT_BINDINGS, 32} {
    CompileShaders();
    BuildLayouts();
    BuildPipelines();
    InitMsrCompositor();
    if (secondary_window) {
        secondary_present_window_ptr = std::make_unique<PresentWindow>(
            *secondary_window, instance, scheduler, IsLowRefreshRate());
    }
}

RendererVulkan::~RendererVulkan() {
    vk::Device device = instance.GetDevice();
    scheduler.Finish();
    main_present_window.WaitPresent();
    device.waitIdle();

    device.destroyShaderModule(present_vertex_shader);
    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        device.destroyPipeline(present_pipelines[i]);
        device.destroyShaderModule(present_shaders[i]);
    }

    for (auto& sampler : present_samplers) {
        device.destroySampler(sampler);
    }

    for (auto& info : screen_infos) {
        device.destroyImageView(info.texture.image_view);
        vmaDestroyImage(instance.GetAllocator(), info.texture.image, info.texture.allocation);
    }

    device.destroyPipeline(cursor_pipeline);
    device.destroyShaderModule(cursor_vertex_shader);
    device.destroyShaderModule(cursor_fragment_shader);
    DestroyMsrCompositor();
}

void RendererVulkan::PrepareRendertarget() {
    const auto& framebuffer_config = pica.regs.framebuffer_config;
    const auto& regs_lcd = pica.regs_lcd;
    for (u32 i = 0; i < 3; i++) {
        const u32 fb_id = i == 2 ? 1 : 0;
        const auto& framebuffer = framebuffer_config[fb_id];
        auto& texture = screen_infos[i].texture;

        const auto color_fill = fb_id == 0 ? regs_lcd.color_fill_top : regs_lcd.color_fill_bottom;
        if (color_fill.is_enabled) {
            screen_infos[i].image_view = texture.image_view;
            FillScreen(color_fill.AsVector(), texture);
            continue;
        }

        if (texture.width != framebuffer.width || texture.height != framebuffer.height ||
            texture.format != framebuffer.color_format) {
            ConfigureFramebufferTexture(texture, framebuffer);
        }

        LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1);
    }
}

void RendererVulkan::PrepareDraw(Frame* frame, const Layout::FramebufferLayout& layout) {
    const auto sampler = present_samplers[!Settings::values.filter_mode.GetValue()];
    const auto present_set = present_heap.Commit();
    for (u32 index = 0; index < screen_infos.size(); index++) {
        update_queue.AddImageSampler(present_set, 0, index, screen_infos[index].image_view,
                                     sampler);
    }

    renderpass_cache.EndRendering();
    scheduler.Record([this, layout, frame, present_set,
                      renderpass = main_present_window.Renderpass(),
                      index = current_pipeline](vk::CommandBuffer cmdbuf) {
        const vk::Viewport viewport = {
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(layout.width),
            .height = static_cast<float>(layout.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };

        const vk::Rect2D scissor = {
            .offset = {0, 0},
            .extent = {layout.width, layout.height},
        };

        cmdbuf.setViewport(0, viewport);
        cmdbuf.setScissor(0, scissor);

        const vk::ClearValue clear{.color = clear_color};
        const vk::PipelineLayout layout{*present_pipeline_layout};
        const vk::RenderPassBeginInfo renderpass_begin_info = {
            .renderPass = renderpass,
            .framebuffer = frame->framebuffer,
            .renderArea =
                vk::Rect2D{
                    .offset = {0, 0},
                    .extent = {frame->width, frame->height},
                },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };

        cmdbuf.beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, present_pipelines[index]);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, present_set, {});
    });
}

void RendererVulkan::RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                                    bool flipped) {
    if (!Settings::values.use_skip_duplicate_frames.GetValue() ||
        Core::PerfStats::game_frames_updated) {
        Frame* frame = window.GetRenderFrame();

        if (layout.width != frame->width || layout.height != frame->height) {
            window.WaitPresent();
            scheduler.Finish();
            window.RecreateFrame(frame, layout.width, layout.height);
        }

        clear_color.float32[0] = Settings::values.bg_red.GetValue();
        clear_color.float32[1] = Settings::values.bg_green.GetValue();
        clear_color.float32[2] = Settings::values.bg_blue.GetValue();
        clear_color.float32[3] = 1.0f;

        DrawScreens(frame, layout, flipped);
        scheduler.Flush(frame->render_ready);
        window.Present(frame);
        if ((secondaryWindowEnabled && isSecondaryWindow) || (!secondaryWindowEnabled)) {
            Core::PerfStats::game_frames_updated = false;
            screenRendered = true;
        }
    }
}

void RendererVulkan::LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info, bool right_eye) {

    if (framebuffer.address_right1 == 0 || framebuffer.address_right2 == 0) {
        right_eye = false;
    }

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0
            ? (right_eye ? framebuffer.address_right1 : framebuffer.address_left1)
            : (right_eye ? framebuffer.address_right2 : framebuffer.address_left2);

    LOG_TRACE(Render_Vulkan, "0x{:08x} bytes from 0x{:08x}({}x{}), fmt {:x}",
              framebuffer.stride * framebuffer.height, framebuffer_addr, framebuffer.width.Value(),
              framebuffer.height.Value(), framebuffer.format);

    const u32 bpp = Pica::BytesPerPixel(framebuffer.color_format);
    const std::size_t pixel_stride = framebuffer.stride / bpp;

    ASSERT(pixel_stride * bpp == framebuffer.stride);
    ASSERT(pixel_stride % 4 == 0);

    if (!rasterizer.AccelerateDisplay(framebuffer, framebuffer_addr, static_cast<u32>(pixel_stride),
                                      screen_info)) {
        // Reset the screen info's display texture to its own permanent texture
        screen_info.image_view = screen_info.texture.image_view;
        screen_info.texcoords = {0.f, 0.f, 1.f, 1.f};

        ASSERT(false);
    }
}

void RendererVulkan::CompileShaders() {
    const vk::Device device = instance.GetDevice();
    const std::string_view preamble =
        instance.IsImageArrayDynamicIndexSupported() ? "#define ARRAY_DYNAMIC_INDEX" : "";
    present_vertex_shader =
        Compile(HostShaders::VULKAN_PRESENT_VERT, vk::ShaderStageFlagBits::eVertex, device);
    present_shaders[0] = Compile(HostShaders::VULKAN_PRESENT_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[1] = Compile(HostShaders::VULKAN_PRESENT_ANAGLYPH_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[2] = Compile(HostShaders::VULKAN_PRESENT_INTERLACED_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);

    cursor_vertex_shader =
        Compile(HostShaders::VULKAN_CURSOR_VERT, vk::ShaderStageFlagBits::eVertex, device);
    cursor_fragment_shader =
        Compile(HostShaders::VULKAN_CURSOR_FRAG, vk::ShaderStageFlagBits::eFragment, device);

    for (std::size_t i = 0; i < present_samplers.size(); i++) {
        const bool linear = i == 0;
        const vk::Filter filter_mode = linear ? vk::Filter::eLinear : vk::Filter::eNearest;
        const vk::SamplerMipmapMode mipmap_mode =
            linear ? vk::SamplerMipmapMode::eLinear : vk::SamplerMipmapMode::eNearest;
        const vk::SamplerCreateInfo sampler_info = {
            .magFilter = filter_mode,
            .minFilter = filter_mode,
            .mipmapMode = mipmap_mode,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0f,
            .compareEnable = false,
            .compareOp = vk::CompareOp::eAlways,
            .minLod = 0.0f,
            .maxLod = 0.0f,
            .borderColor = vk::BorderColor::eIntOpaqueBlack,
            .unnormalizedCoordinates = false,
        };

        present_samplers[i] = device.createSampler(sampler_info);
    }
}

void RendererVulkan::BuildLayouts() {
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(PresentUniformData),
    };

    const auto descriptor_set_layout = present_heap.Layout();
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };
    present_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(layout_info);

    const vk::PipelineLayoutCreateInfo cursor_layout_info = {};
    cursor_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(cursor_layout_info);
}

void RendererVulkan::BuildPipelines() {
    const vk::VertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(ScreenRectVertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };

    const std::array attributes = {
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, position),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, tex_coord),
        },
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleStrip,
        .primitiveRestartEnable = false,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };

    const vk::PipelineColorBlendAttachmentState colorblend_attachment = {
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .attachmentCount = 1,
        .pAttachments = &colorblend_attachment,
    };

    const vk::Viewport placeholder_viewport = vk::Viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const vk::Rect2D placeholder_scissor = vk::Rect2D{{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .viewportCount = 1,
        .pViewports = &placeholder_viewport,
        .scissorCount = 1,
        .pScissors = &placeholder_scissor,
    };

    const std::array dynamic_states = {
        vk::DynamicState::eBlendConstants,
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::PipelineDepthStencilStateCreateInfo depth_info = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
        .depthBoundsTestEnable = false,
        .stencilTestEnable = false,
    };

    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        const std::array shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = present_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = present_shaders[i],
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo pipeline_info = {
            .stageCount = static_cast<u32>(shader_stages.size()),
            .pStages = shader_stages.data(),
            .pVertexInputState = &vertex_input_info,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_info,
            .pRasterizationState = &raster_state,
            .pMultisampleState = &multisampling,
            .pDepthStencilState = &depth_info,
            .pColorBlendState = &color_blending,
            .pDynamicState = &dynamic_info,
            .layout = *present_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build present pipelines");
        present_pipelines[i] = pipeline;
    }

    // Build cursor pipeline (simple position-only, inverted color blending)
    {
        const vk::VertexInputBindingDescription cursor_binding = {
            .binding = 0,
            .stride = sizeof(float) * 2,
            .inputRate = vk::VertexInputRate::eVertex,
        };

        const vk::VertexInputAttributeDescription cursor_attribute = {
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = 0,
        };

        const vk::PipelineVertexInputStateCreateInfo cursor_vertex_input = {
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &cursor_binding,
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions = &cursor_attribute,
        };

        const vk::PipelineInputAssemblyStateCreateInfo cursor_input_assembly = {
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = false,
        };

        const vk::PipelineRasterizationStateCreateInfo cursor_raster = {
            .depthClampEnable = false,
            .rasterizerDiscardEnable = false,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eClockwise,
            .depthBiasEnable = false,
            .lineWidth = 1.0f,
        };

        const vk::PipelineMultisampleStateCreateInfo cursor_multisample = {
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .sampleShadingEnable = false,
        };

        const vk::PipelineColorBlendAttachmentState cursor_blend_attachment = {
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eOneMinusDstColor,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eZero,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };

        const vk::PipelineColorBlendStateCreateInfo cursor_color_blending = {
            .logicOpEnable = false,
            .attachmentCount = 1,
            .pAttachments = &cursor_blend_attachment,
        };

        const vk::Viewport placeholder_vp = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const vk::Rect2D placeholder_sc = {{0, 0}, {1, 1}};
        const vk::PipelineViewportStateCreateInfo cursor_viewport = {
            .viewportCount = 1,
            .pViewports = &placeholder_vp,
            .scissorCount = 1,
            .pScissors = &placeholder_sc,
        };

        const std::array cursor_dynamic_states = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };

        const vk::PipelineDynamicStateCreateInfo cursor_dynamic = {
            .dynamicStateCount = static_cast<u32>(cursor_dynamic_states.size()),
            .pDynamicStates = cursor_dynamic_states.data(),
        };

        const vk::PipelineDepthStencilStateCreateInfo cursor_depth = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = vk::CompareOp::eAlways,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = false,
        };

        const std::array cursor_shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = cursor_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = cursor_fragment_shader,
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo cursor_pipeline_info = {
            .stageCount = static_cast<u32>(cursor_shader_stages.size()),
            .pStages = cursor_shader_stages.data(),
            .pVertexInputState = &cursor_vertex_input,
            .pInputAssemblyState = &cursor_input_assembly,
            .pViewportState = &cursor_viewport,
            .pRasterizationState = &cursor_raster,
            .pMultisampleState = &cursor_multisample,
            .pDepthStencilState = &cursor_depth,
            .pColorBlendState = &cursor_color_blending,
            .pDynamicState = &cursor_dynamic,
            .layout = *cursor_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, cursor_pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build cursor pipeline");
        cursor_pipeline = pipeline;
    }
}

void RendererVulkan::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const Pica::FramebufferConfig& framebuffer) {
    vk::Device device = instance.GetDevice();
    if (texture.image_view) {
        device.destroyImageView(texture.image_view);
    }
    if (texture.image) {
        vmaDestroyImage(instance.GetAllocator(), texture.image, texture.allocation);
    }

    const VideoCore::PixelFormat pixel_format =
        VideoCore::PixelFormatFromGPUPixelFormat(framebuffer.color_format);
    const vk::Format format = instance.GetTraits(pixel_format).native;
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {framebuffer.width, framebuffer.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &texture.allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }
    texture.image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = texture.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    texture.image_view = device.createImageView(view_info);

    texture.width = framebuffer.width;
    texture.height = framebuffer.height;
    texture.format = framebuffer.color_format;
}

void RendererVulkan::FillScreen(Common::Vec3<u8> color, const TextureInfo& texture) {
    // When loading some 3GX extensions, FillScreen may be called before texture image is available
    if (!texture.image) {
        return;
    }

    const vk::ClearColorValue clear_color = {
        .float32 =
            std::array{
                color.r() / 255.0f,
                color.g() / 255.0f,
                color.b() / 255.0f,
                1.0f,
            },
    };

    renderpass_cache.EndRendering();
    scheduler.Record([image = texture.image, clear_color](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };

        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        const vk::ImageMemoryBarrier post_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, clear_color, range);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eFragmentShader,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
    });
}

void RendererVulkan::ReloadPipeline(Settings::StereoRenderOption render_3d) {
    switch (render_3d) {
    case Settings::StereoRenderOption::Anaglyph:
        current_pipeline = 1;
        break;
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced:
        current_pipeline = 2;
        draw_info.reverse_interlaced = render_3d == Settings::StereoRenderOption::ReverseInterlaced;
        break;
    default:
        current_pipeline = 0;
        break;
    }
}

void RendererVulkan::DrawSingleScreen(u32 screen_id, float x, float y, float w, float h,
                                      Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info = screen_infos[screen_id];
    const auto& texcoords = screen_info.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info.texture.width * scale_factor),
                        static_cast<f32>(screen_info.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y,
                                            float w, float h,
                                            Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info_l = screen_infos[screen_id_l];
    const auto& texcoords = screen_info_l.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        static_cast<f32>(screen_info_l.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id_l;
    draw_info.screen_id_r = screen_id_r;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::ApplySecondLayerOpacity(float alpha) {
    scheduler.Record([alpha](vk::CommandBuffer cmdbuf) {
        const std::array<float, 4> blend_constants = {0.0f, 0.0f, 0.0f, alpha};
        cmdbuf.setBlendConstants(blend_constants.data());
    });
}

void RendererVulkan::DrawTopScreen(const Layout::FramebufferLayout& layout,
                                   const Common::Rectangle<u32>& top_screen) {
    if (!layout.top_screen_enabled) {
        return;
    }
    int leftside, rightside;
    leftside = Settings::values.swap_eyes_3d.GetValue() ? 1 : 0;
    rightside = Settings::values.swap_eyes_3d.GetValue() ? 0 : 1;
    const float top_screen_left = static_cast<float>(top_screen.left);
    const float top_screen_top = static_cast<float>(top_screen.top);
    const float top_screen_width = static_cast<float>(top_screen.GetWidth());
    const float top_screen_height = static_cast<float>(top_screen.GetHeight());

    const auto orientation = layout.is_rotated ? Layout::DisplayOrientation::Landscape
                                               : Layout::DisplayOrientation::Portrait;
    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        const int eye = static_cast<int>(Settings::values.mono_render_option.GetValue());
        DrawSingleScreen(eye, top_screen_left, top_screen_top, top_screen_width, top_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySide: {
        DrawSingleScreen(leftside, top_screen_left / 2, top_screen_top, top_screen_width / 2,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, static_cast<float>((top_screen_left / 2) + (layout.width / 2)),
                         top_screen_top, top_screen_width / 2, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, top_screen_left + layout.width / 2, top_screen_top,
                         top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            rightside,
            static_cast<float>(layout.cardboard.top_screen_right_eye + (layout.width / 2)),
            top_screen_top, top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(leftside, rightside, top_screen_left, top_screen_top,
                               top_screen_width, top_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawBottomScreen(const Layout::FramebufferLayout& layout,
                                      const Common::Rectangle<u32>& bottom_screen) {
    if (!layout.bottom_screen_enabled) {
        return;
    }

    const float bottom_screen_left = static_cast<float>(bottom_screen.left);
    const float bottom_screen_top = static_cast<float>(bottom_screen.top);
    const float bottom_screen_width = static_cast<float>(bottom_screen.GetWidth());
    const float bottom_screen_height = static_cast<float>(bottom_screen.GetHeight());

    const auto orientation = layout.is_rotated ? Layout::DisplayOrientation::Landscape
                                               : Layout::DisplayOrientation::Portrait;

    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);

        break;
    }
    case Settings::StereoRenderOption::SideBySide: // Bottom screen is identical on both sides
    {
        DrawSingleScreen(2, bottom_screen_left / 2, bottom_screen_top, bottom_screen_width / 2,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, static_cast<float>((bottom_screen_left / 2) + (layout.width / 2)),
                         bottom_screen_top, bottom_screen_width / 2, bottom_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, bottom_screen_left + layout.width / 2, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            2, static_cast<float>(layout.cardboard.bottom_screen_right_eye + (layout.width / 2)),
            bottom_screen_top, bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(2, 2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                               bottom_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawScreens(Frame* frame, const Layout::FramebufferLayout& layout,
                                 bool flipped) {
    if (settings.bg_color_update_requested.exchange(false)) {
        clear_color.float32[0] = Settings::values.bg_red.GetValue();
        clear_color.float32[1] = Settings::values.bg_green.GetValue();
        clear_color.float32[2] = Settings::values.bg_blue.GetValue();
    }
    if (settings.shader_update_requested.exchange(false)) {
        ReloadPipeline(layout.render_3d_mode);
    }

    if (MSR::Presentation::enabled.load(std::memory_order_relaxed)) {
        DrawMsrScreens(frame, layout);
        return;
    }

    PrepareDraw(frame, layout);

    const auto& top_screen = layout.top_screen;
    const auto& bottom_screen = layout.bottom_screen;
    draw_info.modelview = MakeOrthographicMatrix(layout.width, layout.height);

    draw_info.layer = 0;

    // Apply the initial default opacity value; Needed to avoid flickering
    ApplySecondLayerOpacity(1.0f);

    if (!Settings::values.swap_screen.GetValue()) {
        DrawTopScreen(layout, top_screen);
        draw_info.layer = 0;
        if (layout.bottom_opacity < 1) {
            ApplySecondLayerOpacity(layout.bottom_opacity);
        }
        DrawBottomScreen(layout, bottom_screen);
    } else {
        DrawBottomScreen(layout, bottom_screen);
        draw_info.layer = 0;
        if (layout.top_opacity < 1) {
            ApplySecondLayerOpacity(layout.top_opacity);
        }
        DrawTopScreen(layout, top_screen);
    }

    if (layout.additional_screen_enabled) {
        const auto& additional_screen = layout.additional_screen;
        if (!layout.additional_screen_is_bottom) {
            DrawTopScreen(layout, additional_screen);
        } else {
            DrawBottomScreen(layout, additional_screen);
        }
    }

    DrawCursor(layout);

    scheduler.Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRenderPass(); });
}

void RendererVulkan::DrawCursor(const Layout::FramebufferLayout& layout) {
    const auto cursor = render_window.GetCursorInfo();
    if (!cursor.visible) {
        return;
    }

    const float buf_w = static_cast<float>(layout.width);
    const float buf_h = static_cast<float>(layout.height);

    // Convert from bottom-screen-local to layout-absolute, then to NDC
    const float abs_x = layout.bottom_screen.left + cursor.projected_x;
    const float abs_y = layout.bottom_screen.top + cursor.projected_y;
    const float cx = (abs_x / buf_w) * 2.0f - 1.0f;
    const float cy = (abs_y / buf_h) * 2.0f - 1.0f;
    const float ratio = static_cast<float>(layout.bottom_screen.GetHeight()) / 30.0f;
    const float rw = ratio / buf_w;
    const float rh = ratio / buf_h;

    // Bottom screen bounds in NDC
    const float bl = (layout.bottom_screen.left / buf_w) * 2.0f - 1.0f;
    const float bt = (layout.bottom_screen.top / buf_h) * 2.0f - 1.0f;
    const float br = (layout.bottom_screen.right / buf_w) * 2.0f - 1.0f;
    const float bb = (layout.bottom_screen.bottom / buf_h) * 2.0f - 1.0f;

    // Crosshair geometry clamped to bottom screen bounds
    const float vl = std::fmax(cx - rw / 5.0f, bl);
    const float vr = std::fmin(cx + rw / 5.0f, br);
    const float vt = std::fmax(cy - rh, bt);
    const float vb = std::fmin(cy + rh, bb);

    const float hl = std::fmax(cx - rw, bl);
    const float hr = std::fmin(cx + rw, br);
    const float ht = std::fmax(cy - rh / 5.0f, bt);
    const float hb = std::fmin(cy + rh / 5.0f, bb);

    // 12 vertices = 4 triangles (2 for vertical bar, 2 for horizontal bar)
    // clang-format off
    const float vertices[] = {
        // Vertical bar
        vl, vt,  vr, vt,  vr, vb,
        vl, vt,  vr, vb,  vl, vb,
        // Horizontal bar
        hl, ht,  hr, ht,  hr, hb,
        hl, ht,  hr, hb,  hl, hb,
    };
    // clang-format on

    const u64 size = sizeof(vertices);
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices, size);
    vertex_buffer.Commit(size);

    scheduler.Record([this, offset = offset, pipeline = cursor_pipeline](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        const u32 first_vertex = static_cast<u32>(offset) / (sizeof(float) * 2);
        cmdbuf.draw(12, 1, first_vertex, 0);
    });
}

void RendererVulkan::SwapBuffers() {
    MSR::DrawProbe::NextFrame();
    if (MSR::DrawProbe::Active()) {
        for (u32 i = 0; i < 2; ++i) {
            const auto& config = pica.regs.framebuffer_config[i];
            MSR::DrawProbe::Write(fmt::format("screen{} left1={:08X} left2={:08X} active={} {}x{}",
                                              i, config.address_left1, config.address_left2,
                                              config.active_fb, config.width.Value(),
                                              config.height.Value()));
        }
    }
    system.perf_stats->StartSwap();
    screenRendered = false;
#ifndef ANDROID
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#endif

#ifdef ANDROID
    if (secondary_window) {
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#endif

    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();
    PrepareRendertarget();
    RenderScreenshot();
    isSecondaryWindow = false;
    RenderToWindow(main_present_window, layout, false);
#ifndef ANDROID
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        isSecondaryWindow = true;
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif

#ifdef ANDROID
    if (secondary_window) {
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        isSecondaryWindow = true;
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif
    if (!screenRendered) {
        scheduler.Finish();
    }

    system.perf_stats->EndSwap();
    rasterizer.TickFrame();
    EndFrame();
}

void RendererVulkan::RenderScreenshot() {
    if (!settings.screenshot_requested.exchange(false)) {
        return;
    }

    if (!TryRenderScreenshotWithHostMemory()) {
        RenderScreenshotWithStagingCopy();
    }

    settings.screenshot_complete_callback(false);
}

void RendererVulkan::RenderScreenshotWithStagingCopy() {
    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    const vk::BufferCreateInfo staging_buffer_info = {
        .size = width * height * 4,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
    };

    const VmaAllocationCreateInfo alloc_create_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkBuffer unsafe_buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info;
    VkBufferCreateInfo unsafe_buffer_info = static_cast<VkBufferCreateInfo>(staging_buffer_info);

    VkResult result = vmaCreateBuffer(instance.GetAllocator(), &unsafe_buffer_info,
                                      &alloc_create_info, &unsafe_buffer, &allocation, &alloc_info);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }

    vk::Buffer staging_buffer{unsafe_buffer};

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, false);

    scheduler.Record(
        [width, height, source_image = frame.image, staging_buffer](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::ImageMemoryBarrier write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            static constexpr vk::MemoryBarrier memory_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            };

            const vk::BufferImageCopy image_copy = {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {0, 0, 0},
                .imageExtent = {width, height, 1},
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
            cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                     staging_buffer, image_copy);
            cmdbuf.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
        });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // Copy backing image data to the QImage screenshot buffer
    std::memcpy(settings.screenshot_bits, alloc_info.pMappedData, staging_buffer_info.size);

    // QImage::Format_RGB32 expects BGRA byte order. If the swapchain format is RGBA,
    // swap R and B channels so the screenshot colors are correct.
    if (main_present_window.GetSurfaceFormat() == vk::Format::eR8G8B8A8Unorm) {
        u8* pixels = static_cast<u8*>(settings.screenshot_bits);
        for (u32 i = 0; i < width * height; i++) {
            std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
        }
    }

    // Destroy allocated resources
    vmaDestroyBuffer(instance.GetAllocator(), staging_buffer, allocation);
    vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);
}

bool RendererVulkan::TryRenderScreenshotWithHostMemory() {
    // If the host-memory import alignment matches the allocation granularity of the platform, then
    // the entire span of memory can be trivially imported
    const bool trivial_import =
        instance.IsExternalMemoryHostSupported() &&
        instance.GetMinImportedHostPointerAlignment() == Common::GetPageSize();
    if (!trivial_import) {
        return false;
    }

    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    // For a span of memory [x, x + s], import [AlignDown(x, alignment), AlignUp(x + s, alignment)]
    // and maintain an offset to the start of the data
    const u64 import_alignment = instance.GetMinImportedHostPointerAlignment();
    const uintptr_t address = reinterpret_cast<uintptr_t>(settings.screenshot_bits);
    void* aligned_pointer = reinterpret_cast<void*>(Common::AlignDown(address, import_alignment));
    const u64 offset = address % import_alignment;
    const u64 aligned_size = Common::AlignUp(offset + width * height * 4ull, import_alignment);

    // Buffer<->Image mapping for the imported imported buffer
    const vk::BufferImageCopy buffer_image_copy = {
        .bufferOffset = offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };

    const vk::MemoryHostPointerPropertiesEXT import_properties =
        device.getMemoryHostPointerPropertiesEXT(
            vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, aligned_pointer);

    if (!import_properties.memoryTypeBits) {
        // Could not import memory
        return false;
    }

    const std::optional<u32> memory_type_index = FindMemoryType(
        instance.GetPhysicalDevice().getMemoryProperties(),
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        import_properties.memoryTypeBits);

    if (!memory_type_index.has_value()) {
        // Could not find memory type index
        return false;
    }

    const vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportMemoryHostPointerInfoEXT>
        allocation_chain = {
            vk::MemoryAllocateInfo{
                .allocationSize = aligned_size,
                .memoryTypeIndex = memory_type_index.value(),
            },
            vk::ImportMemoryHostPointerInfoEXT{
                .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
                .pHostPointer = aligned_pointer,
            },
        };

    // Import host memory
    const vk::UniqueDeviceMemory imported_memory =
        device.allocateMemoryUnique(allocation_chain.get());

    const vk::StructureChain<vk::BufferCreateInfo, vk::ExternalMemoryBufferCreateInfo> buffer_info =
        {
            vk::BufferCreateInfo{
                .size = aligned_size,
                .usage = vk::BufferUsageFlagBits::eTransferDst,
                .sharingMode = vk::SharingMode::eExclusive,
            },
            vk::ExternalMemoryBufferCreateInfo{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
            },
        };

    // Bind imported memory to buffer
    const vk::UniqueBuffer imported_buffer = device.createBufferUnique(buffer_info.get());
    device.bindBufferMemory(imported_buffer.get(), imported_memory.get(), 0);

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, false);

    scheduler.Record([buffer_image_copy, source_image = frame.image,
                      imported_buffer = imported_buffer.get()](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier read_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        const vk::ImageMemoryBarrier write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        static constexpr vk::MemoryBarrier memory_write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
        cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                 imported_buffer, buffer_image_copy);
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
            vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
    });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // QImage::Format_RGB32 expects BGRA byte order. If the swapchain format is RGBA,
    // swap R and B channels so the screenshot colors are correct.
    if (main_present_window.GetSurfaceFormat() == vk::Format::eR8G8B8A8Unorm) {
        u8* pixels = static_cast<u8*>(settings.screenshot_bits);
        for (u32 i = 0; i < width * height; i++) {
            std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
        }
    }

    // Image data has been copied directly to host memory
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);

    return true;
}

void RendererVulkan::NotifySurfaceChanged(bool is_second_window) {
    if (is_second_window) {
        if (secondary_present_window_ptr) {
            secondary_present_window_ptr->NotifySurfaceChanged();
        }
    } else {
        main_present_window.NotifySurfaceChanged();
    }
}


// Samus Returns single-screen compositor. The top screen fills a 16:9 view; the gameplay HUD,
// which the game draws on the bottom screen, is composited onto it in a Metroid Dread style
// arrangement, and the whole bottom screen appears as a panel for the pause menu, file select
// and prompts. A 320x240 readback of the bottom screen tells which of these is showing.

namespace {

constexpr u32 MsrProbeWidth = 320;
constexpr u32 MsrProbeHeight = 240;

/// The minimap's window on the bottom screen, in its pixels.
constexpr f32 MsrMapWindowWidth = 120.f;
constexpr f32 MsrMapWindowHeight = 80.f;

/// Normalized source rectangle from native bottom-screen pixels.
Common::Rectangle<f32> MsrSource(f32 x0, f32 y0, f32 x1, f32 y1) {
    return {x0 / MsrProbeWidth, y0 / MsrProbeHeight, x1 / MsrProbeWidth, y1 / MsrProbeHeight};
}

f32 MsrApproach(f32 value, f32 target, f32 step) {
    return value < target ? std::min(target, value + step) : std::max(target, value - step);
}

/// Pieces fade in and out in about a tenth of a second.
constexpr f32 MsrFadePerSecond = 9.f;
constexpr f32 MsrPromptFadePerSecond = 6.f;

} // Anonymous namespace

void RendererVulkan::InitMsrCompositor() {
    const vk::Device device = instance.GetDevice();
    const std::string_view preamble =
        instance.IsImageArrayDynamicIndexSupported() ? "#define ARRAY_DYNAMIC_INDEX" : "";
    msr_shader = Compile(HostShaders::VULKAN_PRESENT_MSR_FRAG, vk::ShaderStageFlagBits::eFragment,
                         device, preamble);

    const vk::VertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(ScreenRectVertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };
    const std::array attributes = {
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, position),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, tex_coord),
        },
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleStrip,
        .primitiveRestartEnable = false,
    };
    const vk::PipelineRasterizationStateCreateInfo raster_state = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };
    // The shader computes coverage (key and opacity) into alpha.
    const vk::PipelineColorBlendAttachmentState colorblend_attachment = {
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .attachmentCount = 1,
        .pAttachments = &colorblend_attachment,
    };
    const vk::Viewport placeholder_viewport = vk::Viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const vk::Rect2D placeholder_scissor = vk::Rect2D{{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .viewportCount = 1,
        .pViewports = &placeholder_viewport,
        .scissorCount = 1,
        .pScissors = &placeholder_scissor,
    };
    const std::array dynamic_states = {
        vk::DynamicState::eBlendConstants,
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };
    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_info = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
        .depthBoundsTestEnable = false,
        .stencilTestEnable = false,
    };
    const std::array shader_stages = {
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = present_vertex_shader,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = msr_shader,
            .pName = "main",
        },
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_info,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *present_pipeline_layout,
        .renderPass = main_present_window.Renderpass(),
    };
    const auto [result, pipeline] = device.createGraphicsPipeline({}, pipeline_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build the Samus Returns pipeline");
    msr_pipeline = pipeline;

    if (auto* overlay = MSR::OverlayHook::overlay.load()) {
        const MSR::OverlayHook::VulkanContext context{
            .api_version = TargetVulkanApiVersion,
            .instance = instance.GetInstance(),
            .physical_device = instance.GetPhysicalDevice(),
            .device = device,
            .queue_family = instance.GetGraphicsQueueFamilyIndex(),
            .queue = instance.GetGraphicsQueue(),
            .render_pass = main_present_window.Renderpass(),
            .image_count = main_present_window.ImageCount(),
            .get_instance_proc_addr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
            .queue_mutex = &scheduler.submit_mutex,
        };
        overlay->Init(context);
    }
}

void RendererVulkan::DestroyMsrCompositor() {
    const vk::Device device = instance.GetDevice();
    if (auto* overlay = MSR::OverlayHook::overlay.load()) {
        overlay->Shutdown();
    }
    device.destroyPipeline(msr_pipeline);
    device.destroyShaderModule(msr_shader);
    const auto destroy_readbacks = [this](auto& readbacks) {
        for (auto& readback : readbacks) {
            if (readback.buffer) {
                vmaDestroyBuffer(instance.GetAllocator(), readback.buffer, readback.allocation);
            }
        }
    };
    destroy_readbacks(msr_readbacks);
    destroy_readbacks(msr_top_readbacks);
    for (Frame* probe : {&msr_probe, &msr_top_probe}) {
        if (probe->framebuffer) {
            device.destroyFramebuffer(probe->framebuffer);
            device.destroyImageView(probe->image_view);
            vmaDestroyImage(instance.GetAllocator(), probe->image, probe->allocation);
        }
    }
}

void RendererVulkan::DrawMsrRegion(u32 screen_id, const Common::Rectangle<f32>& source, f32 x,
                                   f32 y, f32 w, f32 h, const MsrStyle& style) {
    // Screens are stored rotated: screen x runs along the texture's v axis, screen y along u.
    const auto& texcoords = screen_infos[screen_id].texcoords;
    const auto u = [&](f32 sy) { return texcoords.bottom + sy * (texcoords.top - texcoords.bottom); };
    const auto v = [&](f32 sx) { return texcoords.left + sx * (texcoords.right - texcoords.left); };
    const std::array vertices = {
        ScreenRectVertex(x, y, u(source.top), v(source.left)),
        ScreenRectVertex(x + w, y, u(source.top), v(source.right)),
        ScreenRectVertex(x, y + h, u(source.bottom), v(source.left)),
        ScreenRectVertex(x + w, y + h, u(source.bottom), v(source.right)),
    };
    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    PresentUniformData info = draw_info;
    const f32 mode = style.over_black ? 3.f : style.keyed ? 1.f : 0.f;
    info.i_resolution = Common::MakeVec(style.opacity, style.over_black ? style.gain : style.key_low,
                                        style.key_high, mode);
    info.o_resolution = Common::MakeVec(u(source.top), v(source.left), u(source.bottom),
                                        v(source.right));
    // The shader's x runs down the screen (u), its y across (v).
    info.layer = style.feather;
    if (style.hard_borders) {
        info.layer |= (source.top <= 0.f ? 0x100 : 0) | (source.left <= 0.f ? 0x200 : 0) |
                      (source.bottom >= 1.f ? 0x400 : 0) | (source.right >= 1.f ? 0x800 : 0);
    }
    info.reverse_interlaced = style.circle;
    info.screen_id_l = static_cast<int>(screen_id);
    scheduler.Record([this, offset = offset, info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);
        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::DrawMsrFill(f32 x, f32 y, f32 w, f32 h, const Common::Vec4f& color,
                                 int feather) {
    // Mode 2 ignores the texture; the coordinates carry the position inside the quad.
    const std::array vertices = {
        ScreenRectVertex(x, y, 0.f, 0.f),
        ScreenRectVertex(x + w, y, 1.f, 0.f),
        ScreenRectVertex(x, y + h, 0.f, 1.f),
        ScreenRectVertex(x + w, y + h, 1.f, 1.f),
    };
    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    PresentUniformData info = draw_info;
    info.i_resolution = Common::MakeVec(1.f, 0.f, 0.f, 2.f);
    info.o_resolution = color;
    info.layer = feather;
    scheduler.Record([this, offset = offset, info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);
        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::DrawMsrScreens(Frame* frame, const Layout::FramebufferLayout& layout) {
    namespace P = MSR::Presentation;
    ConsumeMsrReadbacks();

    PrepareDraw(frame, layout);
    draw_info.modelview = MakeOrthographicMatrix(layout.width, layout.height);
    draw_info.layer = 0;
    ApplySecondLayerOpacity(1.0f);

    const auto state = static_cast<P::Bottom>(P::bottom.load(std::memory_order_relaxed));
    const bool forced = P::force_panel.load(std::memory_order_relaxed);

    // Fades follow emulated time, so they look the same at every frame rate.
    const s64 now_us = system.CoreTiming().GetGlobalTimeUs().count();
    const f32 seconds =
        msr_last_us != 0 ? std::clamp(static_cast<f32>(now_us - msr_last_us) / 1e6f, 0.f, .1f)
                         : 0.f;
    msr_last_us = now_us;

    // Start in gameplay opens the pause menu. It opens and closes over a still-black bottom
    // screen where its text, then the HUD's pieces, show before the rest and would read as a
    // prompt, and the readback runs a frame or two behind. So the HUD goes as soon as Start
    // reaches the game, nothing else shows until the menu does, and once it closes no prompt
    // shows until the HUD is back.
    constexpr s64 transition_us = 1'500'000;
    const long long start_us = P::start_pressed_us.load(std::memory_order_relaxed);
    if (start_us != msr_start_seen) {
        msr_start_seen = start_us;
        if (state == P::Bottom::Hud && !forced) {
            msr_pausing_until_us = start_us + transition_us;
        }
    }
    if (msr_pausing_until_us != 0 && now_us >= msr_pausing_until_us) {
        msr_pausing_until_us = 0;
    }
    if (state == P::Bottom::Menu && msr_pausing_until_us != 0) {
        msr_pausing_until_us = 0;
        msr_paused = true;
    }
    if (msr_paused && state != P::Bottom::Menu) {
        msr_paused = false;
        msr_resuming_until_us = now_us + transition_us;
    }
    if (state == P::Bottom::Hud) {
        msr_resuming_until_us = 0;
    }
    const bool pausing = msr_pausing_until_us != 0;
    const bool resuming = now_us < msr_resuming_until_us;

    // Menus fading in or out (their text before their frame, or over a backdrop-less screen) can
    // read as a prompt for a few frames; real prompts stay for seconds. A prompt shows once it
    // has held for a moment. The title's "Press A to Start" pulses, and while dim it reads as
    // the bare emblem, so a prompt on screen stays through short gaps.
    if (state != P::Bottom::Prompt) {
        msr_prompt_since_us = now_us;
    } else {
        msr_prompt_seen_us = now_us;
    }
    // The camera zoom holds in play: from the HUD until the title prompt (held 0.3 s, as menus
    // fading can read as a prompt for a moment) or the file select, so the pause menu and
    // cutscenes keep it and the title screens are never zoomed. It eases in and out.
    const auto page = static_cast<P::Page>(P::page.load(std::memory_order_relaxed));
    if (state == P::Bottom::Hud) {
        msr_in_play = true;
    } else if ((state == P::Bottom::Prompt && now_us - msr_prompt_since_us >= 300'000) ||
               (state == P::Bottom::Menu &&
                (page == P::Page::FileList || page == P::Page::FileOpen ||
                 page == P::Page::Gallery))) {
        msr_in_play = false;
    }
    const f32 zoom_target = msr_in_play ? P::zoom.load(std::memory_order_relaxed) : 1.f;
    msr_zoom += (zoom_target - msr_zoom) * (1.f - std::exp(-8.f * seconds));
    if (std::abs(zoom_target - msr_zoom) < .001f) {
        msr_zoom = zoom_target;
    }
    P::world_zoom.store(msr_zoom, std::memory_order_relaxed);

    const bool prompt_on = msr_prompt_alpha > 0.f;
    const bool prompt =
        (state == P::Bottom::Prompt &&
         (prompt_on || now_us - msr_prompt_since_us >= 300'000)) ||
        (prompt_on && state == P::Bottom::Logo && now_us - msr_prompt_seen_us < 1'500'000);
    const bool show_hud = state == P::Bottom::Hud && !forced && !pausing;
    const bool show_panel = state == P::Bottom::Menu || forced;
    const bool show_prompt = prompt && !forced && !pausing && !resuming &&
                             now_us - msr_prompt_foreign_us >= 300'000;
    const f32 fade = MsrFadePerSecond * seconds;
    msr_hud_alpha = MsrApproach(msr_hud_alpha, show_hud ? 1.f : 0.f, fade);
    msr_panel_alpha = MsrApproach(msr_panel_alpha, show_panel ? 1.f : 0.f, fade);
    msr_prompt_alpha = MsrApproach(msr_prompt_alpha, show_prompt ? 1.f : 0.f,
                                   MsrPromptFadePerSecond * seconds);
    P::panel_visible = msr_panel_alpha > 0.5f || msr_prompt_alpha > 0.5f;

    // Gallery viewer: the art is on the top screen and only its page counter on the bottom, so
    // the view moves aside and the panel stands next to it instead of over the art.
    const bool gallery =
        state == P::Bottom::Menu &&
        static_cast<P::Page>(P::page.load(std::memory_order_relaxed)) == P::Page::Gallery;
    msr_gallery = MsrApproach(msr_gallery, gallery ? 1.f : 0.f, fade);
    const f32 ease = msr_gallery * msr_gallery * (3.f - 2.f * msr_gallery);
    Layout::FramebufferLayout view = layout;
    if (ease > 0.f) {
        const auto& full = layout.top_screen;
        const f32 fx = static_cast<f32>(full.left), fy = static_cast<f32>(full.top);
        const f32 fw = static_cast<f32>(full.GetWidth()), fh = static_cast<f32>(full.GetHeight());
        const f32 gap = fh * .03f;
        const f32 vw = fw * .72f, vh = fh * .72f;
        const f32 vx = fx + gap, vy = fy + (fh - vh) / 2;
        const f32 room = fx + fw - (vx + vw) - 2 * gap;
        const f32 ph = std::min(room * .75f, vh), pw = ph / .75f;
        const f32 px = vx + vw + gap + (room - pw) / 2, py = fy + (fh - ph) / 2;
        const auto mix = [ease](const Common::Rectangle<u32>& from, f32 x, f32 y, f32 w, f32 h) {
            const auto lerp = [ease](u32 a, f32 b) {
                return static_cast<u32>(std::lround(static_cast<f32>(a) + (b - a) * ease));
            };
            return Common::Rectangle<u32>{lerp(from.left, x), lerp(from.top, y),
                                          lerp(from.right, x + w), lerp(from.bottom, y + h)};
        };
        view.top_screen = mix(layout.top_screen, vx, vy, vw, vh);
        view.bottom_screen = mix(layout.bottom_screen, px, py, pw, ph);
    }
    DrawTopScreen(view, view.top_screen);

    scheduler.Record([this](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, msr_pipeline);
    });

    const auto& top = layout.top_screen;
    const f32 left = static_cast<f32>(top.left);
    const f32 top_y = static_cast<f32>(top.top);
    const f32 width = static_cast<f32>(top.GetWidth());
    const f32 height = static_cast<f32>(top.GetHeight());
    // One unit is one of the game's own pixels at the current view size.
    const f32 unit = height / 240.f * std::clamp(P::hud_scale.load(), .5f, 2.f);
    const f32 margin = 6.f * unit;

    if (msr_hud_alpha > 0.f) {
        const f32 alpha = msr_hud_alpha * std::clamp(P::hud_opacity.load(), 0.f, 1.f);
        // Keyed: the backdrop (brightness up to ~0.3) disappears, rules and digits stay. No soft
        // edge on the screen's border, where the selector's left icon (Lightning Armor) sits.
        const MsrStyle hud{.opacity = alpha, .keyed = true, .key_low = .28f, .key_high = .5f,
                           .feather = 6, .hard_borders = true};
        const auto piece = [&](f32 x0, f32 y0, f32 x1, f32 y1, f32 x, f32 y, f32 scale) {
            DrawMsrRegion(2, MsrSource(x0, y0, x1, y1), x, y, (x1 - x0) * unit * scale,
                          (y1 - y0) * unit * scale, hud);
        };
        // Soft shade behind the digits keeps them legible over bright scenery, as in Dread.
        const Common::Vec4f shade{0.f, 0.f, 0.f, .32f * alpha};

        // Energy, tanks and ammunition, with the Aeion gauge under them (y 45-50) and the
        // ability selector on their left (down to y 58): upper left, as in Dread.
        constexpr f32 energy_scale = .8f;
        constexpr f32 energy_height = 58.f;
        DrawMsrFill(left + margin, top_y + margin - 2.f * unit * energy_scale,
                    230.f * unit * energy_scale, (energy_height + 4.f) * unit * energy_scale, shade,
                    30);
        piece(0, 0, 250, energy_height, left + margin, top_y + margin, energy_scale);

        // The beam slots in a row under them: the Aeion ability (the usual beam), then the Ice
        // and Grapple Beams once Samus has them. Only their icons: a circle inside the slot's
        // octagonal frame masks it and the key removes the slot's striped fill, which is as
        // bright as the unlit icons in a lit slot, so each gets its own key. The lit one glows,
        // as on the bottom screen. Greyed out (Morph Ball) the icons stay in place, dimmed as
        // the game dims them, and the keys follow them down: about half as bright, except the
        // Grapple slot's own fill, which stays.
        if (P::show_abilities) {
            msr_beam_grey = MsrApproach(msr_beam_grey, msr_beams_greyed ? 1.f : 0.f,
                                        6.f * seconds);
            const auto keyed = [&](f32 low, f32 high, f32 grey_low, f32 grey_high) {
                return MsrStyle{.opacity = alpha, .keyed = true,
                                .key_low = low + (grey_low - low) * msr_beam_grey,
                                .key_high = high + (grey_high - high) * msr_beam_grey,
                                .circle = 38};
            };
            const MsrStyle unlit_icon = keyed(.3f, .5f, .18f, .28f);
            const MsrStyle unlit_grapple = keyed(.3f, .5f, .39f, .47f);
            const MsrStyle lit_icon = keyed(.6f, .85f, .35f, .55f);
            const f32 size = 64.f * unit * .6f;
            const f32 y = top_y + margin + (energy_height + 2.f) * unit * energy_scale;
            const int lit = P::beam_slot.load(std::memory_order_relaxed);
            const int present = P::beam_slots.load(std::memory_order_relaxed);
            f32 x = left + margin;
            for (int i = 0; i < static_cast<int>(MSR::BottomClassifier::BeamSlotRects.size()); ++i) {
                if (i > 0 && (present & (1 << i)) == 0) {
                    continue;
                }
                if (i == lit) {
                    DrawMsrFill(x, y, size, size,
                                {.35f, .75f, .95f, .4f * alpha * (1.f - .6f * msr_beam_grey)}, 45);
                }
                const auto& [x0, y0, x1, y1] = MSR::BottomClassifier::BeamSlotRects[i];
                DrawMsrRegion(2,
                              MsrSource(static_cast<f32>(x0), static_cast<f32>(y0),
                                        static_cast<f32>(x1), static_cast<f32>(y1)),
                              x, y, size, size,
                              i == lit ? lit_icon : i == 2 ? unlit_grapple : unlit_icon);
                x += size + 4.f * unit;
            }
        }

        // Map: the area around Samus, upper right, translucent with a frame. Its window keeps her
        // marker in the middle, gliding after it: the game stops scrolling its map near an
        // area's edges and moves the marker instead, a tile at a time. Where the window passes
        // the game's map it shows the map's dark backdrop.
        f32 map_x = left + width - margin;
        f32 map_w = 0.f;
        f32 map_bottom = top_y + margin;
        if (P::show_minimap) {
            const f32 scale = .675f * std::clamp(P::minimap_size.load(), .5f, 2.f);
            constexpr f32 source_w = MsrMapWindowWidth;
            constexpr f32 source_h = MsrMapWindowHeight;
            const f32 w = source_w * unit * scale;
            const f32 h = source_h * unit * scale;
            const f32 x = left + width - margin - w;
            const f32 y = top_y + margin;
            const f32 opacity = alpha * std::clamp(P::minimap_opacity.load(), 0.f, 1.f);
            const MsrStyle map{.opacity = opacity};

            const Common::Vec2f gap = msr_map_marker - msr_map_center;
            if (std::abs(gap.x) + std::abs(gap.y) < .1f) {
                msr_map_center = msr_map_marker;
            } else {
                msr_map_center += gap * (1.f - std::exp(-12.f * seconds));
            }
            const f32 sx0 = msr_map_center.x - source_w / 2;
            const f32 sy0 = msr_map_center.y - source_h / 2;
            const auto& [mx0, my0, mx1, my1] = MSR::BottomClassifier::MapRect;
            const f32 cx0 = std::max(sx0, static_cast<f32>(mx0));
            const f32 cy0 = std::max(sy0, static_cast<f32>(my0));
            const f32 cx1 = std::min(sx0 + source_w, static_cast<f32>(mx1));
            const f32 cy1 = std::min(sy0 + source_h, static_cast<f32>(my1));
            const f32 k = w / source_w; // Window pixels per bottom-screen pixel
            const f32 ix = x + (cx0 - sx0) * k, iy = y + (cy0 - sy0) * k;
            const f32 iw = (cx1 - cx0) * k, ih = (cy1 - cy0) * k;
            const auto backdrop = [&](std::size_t edge) {
                const auto& color = msr_map_backdrop[edge];
                return Common::MakeVec(color.r(), color.g(), color.b(), opacity);
            };
            if (ix > x) {
                DrawMsrFill(x, y, ix - x, h, backdrop(0));
            }
            if (iy > y) {
                DrawMsrFill(ix, y, iw, iy - y, backdrop(1));
            }
            if (ix + iw < x + w) {
                DrawMsrFill(ix + iw, y, x + w - (ix + iw), h, backdrop(2));
            }
            if (iy + ih < y + h) {
                DrawMsrFill(ix, iy + ih, iw, y + h - (iy + ih), backdrop(3));
            }
            DrawMsrRegion(2, MsrSource(cx0, cy0, cx1, cy1), ix, iy, iw, ih, map);
            const f32 line = std::max(1.f, unit * .75f);
            const Common::Vec4f frame_color{.35f, .75f, .95f, .8f * alpha};
            DrawMsrFill(x - line, y - line, w + 2 * line, line, frame_color);
            DrawMsrFill(x - line, y + h, w + 2 * line, line, frame_color);
            DrawMsrFill(x - line, y, line, h, frame_color);
            DrawMsrFill(x + w, y, line, h, frame_color);
            map_x = x;
            map_w = w;
            map_bottom = y + h + line;
        }

        // DNA and remaining-Metroid counters: on the right under the map, no wider than it, so
        // they never cover the game's own messages along the bottom of the screen.
        if (P::show_counters) {
            // Crops stop short of the counter bar's frame: its rule (y 214-215) and angled ends
            // (x 60-63, 240-245). A stronger key drops the bar's striped fill.
            const f32 natural = (33.f + 10.f + 48.f) * unit;
            const f32 fit = map_w > 0.f ? std::min(1.f, map_w / natural) : 1.f;
            const f32 dna_w = 33.f * unit * fit;
            const f32 gap = 10.f * unit * fit;
            const f32 total = natural * fit;
            const f32 h = 23.f * unit * fit;
            const f32 x = map_w > 0.f ? map_x + (map_w - total) / 2 : map_x - total;
            const f32 y = map_bottom + 6.f * unit;
            DrawMsrFill(x - 8.f * unit, y - 3.f * unit, total + 16.f * unit, h + 6.f * unit, shade,
                        30);
            const MsrStyle counter{.opacity = alpha, .keyed = true, .key_low = .38f,
                                   .key_high = .58f, .feather = 6};
            DrawMsrRegion(2, MsrSource(64, 217, 97, 240), x, y, dna_w, h, counter);
            DrawMsrRegion(2, MsrSource(192, 217, 240, 240), x + dna_w + gap, y, 48.f * unit * fit,
                          h, counter);
        }
    }

    const auto& panel = view.bottom_screen;
    if (msr_prompt_alpha > 0.f) {
        // Text on the bare backdrop: floated in the lower part of the view, like subtitles. The
        // game centres it on row ~125: the title's prompt (rows 113-125) and the prologue's
        // pages, up to nine lines (rows 40-211), which it types out line by line.
        //
        // Behind it, the title menus' dark card, around the text: it grows as lines appear and
        // takes a new page's size at once when the first line moves, or once smaller text has
        // held for half a second (the old page fades out as the new one starts; not when the
        // title's prompt dims).
        const auto& typed = msr_prompt_text;
        auto& card = msr_prompt_card;
        const auto beyond = [](const Common::Rectangle<f32>& a, const Common::Rectangle<f32>& b) {
            return a.left < b.left - 1.f || a.top < b.top - 1.f || a.right > b.right + 1.f ||
                   a.bottom > b.bottom + 1.f;
        };
        if (msr_prompt_card_us == 0 || std::abs(typed.top - card.top) > 4.f) {
            if (msr_prompt_card_us == 0) {
                msr_prompt_card_shown = typed;
            }
            card = typed;
            msr_prompt_card_us = now_us;
        } else if (beyond(typed, card)) {
            card = {std::min(card.left, typed.left), std::min(card.top, typed.top),
                    std::max(card.right, typed.right), std::max(card.bottom, typed.bottom)};
            msr_prompt_card_us = now_us;
        } else if (!beyond(card, typed) || now_us - msr_prompt_card_us > 500'000) {
            card = typed;
            msr_prompt_card_us = now_us;
        }
        const f32 pad_x = 12.f, pad_y = 7.f;

        // One line (the title's prompt) at the menu panel's scale, row 120 at 80% of the view.
        // Pages of several lines (the prologue's) ease to a scale that fits rows 36-216 in the
        // lower half as their second line appears, kept while pages follow each other, so the
        // text does not move as it is typed; as does any text running past the view's lower
        // part.
        const f32 panel_scale = static_cast<f32>(panel.GetHeight()) / 240.f;
        const f32 fit_scale =
            std::min({panel_scale, height * .52f / 180.f, width * .94f / 320.f});
        if (card.bottom - card.top > 26.f ||
            height * .8f + (card.bottom + pad_y - 120.f) * panel_scale > height * .95f) {
            msr_prompt_tall_us = now_us;
        }
        const bool tall = msr_prompt_tall_us != 0 && now_us - msr_prompt_tall_us < 5'000'000;
        msr_prompt_fit = MsrApproach(msr_prompt_fit, tall ? 1.f : 0.f, 4.f * seconds);
        const f32 fit = msr_prompt_fit * msr_prompt_fit * (3.f - 2.f * msr_prompt_fit);
        const f32 scale = panel_scale + (fit_scale - panel_scale) * fit;
        const f32 short_y = height * .8f - 120.f * panel_scale;
        const f32 fit_y = std::min(height * .8f, height * .95f - 90.f * fit_scale) - 126.f * fit_scale;
        const f32 w = 320.f * scale;
        const f32 h = 240.f * scale;
        const f32 x = left + (width - w) / 2;
        const f32 y = top_y + short_y + (fit_y - short_y) * fit;

        auto& shown = msr_prompt_card_shown;
        const f32 ease = std::min(1.f, 12.f * seconds);
        shown = {shown.left + (card.left - shown.left) * ease,
                 shown.top + (card.top - shown.top) * ease,
                 shown.right + (card.right - shown.right) * ease,
                 shown.bottom + (card.bottom - shown.bottom) * ease};
        const f32 card_alpha = std::clamp(P::menu_backdrop.load(std::memory_order_relaxed), 0.f, 1.f);
        DrawMsrFill(x + (shown.left - pad_x) * scale, y + (shown.top - pad_y) * scale,
                    (shown.right - shown.left + 2 * pad_x) * scale,
                    (shown.bottom - shown.top + 2 * pad_y) * scale,
                    {.01f, .035f, .07f, card_alpha * msr_prompt_alpha}, 4);
        if (msr_bottom_black) {
            // Text alone on black (the title's prompt, which the game fades from bright to
            // nearly black every 1.5 s): restored to full brightness, breathing with the pulse.
            const f32 peak = std::max(msr_bottom_peak, .05f);
            const MsrStyle text{.opacity = msr_prompt_alpha * (.7f + .3f * std::min(peak / .88f, 1.f)),
                                .over_black = true,
                                .gain = std::clamp(.88f / peak, 1.f, 12.f)};
            DrawMsrRegion(2, {0.f, 0.f, 1.f, 1.f}, x, y, w, h, text);
        } else {
            const MsrStyle text{.opacity = msr_prompt_alpha, .keyed = true, .key_low = .45f,
                                .key_high = .65f, .feather = 4};
            DrawMsrRegion(2, {0.f, 0.f, 1.f, 1.f}, x, y, w, h, text);
        }
    } else {
        msr_prompt_card_us = 0;
        if (now_us - msr_prompt_tall_us >= 5'000'000) {
            msr_prompt_fit = 0.f; // the next prompt starts at the panel's scale
        }
    }
    if (msr_panel_alpha > 0.f) {
        // The title menus float over the game view without their backdrop, on a dark card that
        // lets a little of the view through; other panels keep theirs, over a darkened view.
        // The gallery's view stays bright.
        const bool see_through = P::backdrop_removed.load(std::memory_order_relaxed);
        DrawMsrFill(left, top_y, width, height,
                    {0.f, 0.f, 0.f, (see_through ? .2f : .45f) * msr_panel_alpha * (1.f - ease)});
        if (see_through) {
            const f32 grow = static_cast<f32>(panel.GetHeight()) * .03f;
            const f32 card = std::clamp(P::menu_backdrop.load(std::memory_order_relaxed), 0.f, 1.f);
            DrawMsrFill(static_cast<f32>(panel.left) - grow, static_cast<f32>(panel.top) - grow,
                        static_cast<f32>(panel.GetWidth()) + 2 * grow,
                        static_cast<f32>(panel.GetHeight()) + 2 * grow,
                        {.01f, .035f, .07f, card * msr_panel_alpha}, 4);
        }
        const MsrStyle full{.opacity = msr_panel_alpha, .over_black = see_through};
        DrawMsrRegion(2, {0.f, 0.f, 1.f, 1.f}, static_cast<f32>(panel.left),
                      static_cast<f32>(panel.top), static_cast<f32>(panel.GetWidth()),
                      static_cast<f32>(panel.GetHeight()), full);

        // Touch navigation: a glowing frame around the focused button, or a crosshair cursor.
        if (P::touch_mode.load(std::memory_order_relaxed)) {
            const f32 sx = static_cast<f32>(panel.GetWidth()) / 320.f;
            const f32 sy = static_cast<f32>(panel.GetHeight()) / 240.f;
            const f32 px = static_cast<f32>(panel.left);
            const f32 py = static_cast<f32>(panel.top);
            const Common::Vec4f glow{1.f, .78f, .2f, .95f * msr_panel_alpha};
            const f32 line = std::max(2.f, 1.25f * sx);
            const int x0 = P::touch_x0, y0 = P::touch_y0, x1 = P::touch_x1, y1 = P::touch_y1;
            if (x1 > x0 && y1 > y0) {
                const f32 fx = px + x0 * sx - line, fy = py + y0 * sy - line;
                const f32 fw = (x1 - x0) * sx + 2 * line, fh = (y1 - y0) * sy + 2 * line;
                DrawMsrFill(fx, fy, fw, fh, {1.f, .78f, .2f, .18f * msr_panel_alpha});
                DrawMsrFill(fx, fy, fw, line, glow);
                DrawMsrFill(fx, fy + fh - line, fw, line, glow);
                DrawMsrFill(fx, fy, line, fh, glow);
                DrawMsrFill(fx + fw - line, fy, line, fh, glow);
            } else {
                const f32 cx = px + P::touch_x * sx, cy = py + P::touch_y * sy;
                const f32 arm = 7.f * sx;
                DrawMsrFill(cx - arm, cy - line / 2, 2 * arm, line, glow);
                DrawMsrFill(cx - line / 2, cy - arm, line, 2 * arm, glow);
            }
        }
    }
    P::panel_x0 = static_cast<int>(panel.left);
    P::panel_y0 = static_cast<int>(panel.top);
    P::panel_x1 = static_cast<int>(panel.right);
    P::panel_y1 = static_cast<int>(panel.bottom);

    DrawCursor(layout);
    // The settings overlay goes on top of everything, inside the same render pass.
    if (auto* overlay = MSR::OverlayHook::overlay.load()) {
        scheduler.Record([overlay, width = layout.width, height = layout.height](
                             vk::CommandBuffer cmdbuf) { overlay->Draw(cmdbuf, width, height); });
    }
    scheduler.Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRenderPass(); });

    ReadBackMsrBottom();
    ReadBackMsrTop();
}

void RendererVulkan::ReadBackMsrBottom() {
    auto& readback = msr_readbacks[msr_readback_next];
    if (readback.pending) {
        return; // The GPU has not finished with this slot yet; classify a little later.
    }
    if (RecordMsrReadback(readback, msr_probe, 2, MsrProbeWidth, MsrProbeHeight)) {
        msr_readback_next = (msr_readback_next + 1) % static_cast<u32>(msr_readbacks.size());
    }
}

void RendererVulkan::ReadBackMsrTop() {
    ConsumeMsrTopReadbacks();
    if (msr_top_left == 0) {
        return;
    }
    auto& readback = msr_top_readbacks[msr_top_next];
    if (readback.pending) {
        // Every frame is wanted: wait for the oldest copy rather than skip this one.
        scheduler.Finish();
        ConsumeMsrTopReadbacks();
    }
    if (RecordMsrReadback(readback, msr_top_probe, 0, Core::kScreenTopWidth,
                          Core::kScreenTopHeight)) {
        readback.frame = MSR::DrawProbe::frame;
        --msr_top_left;
        msr_top_next = (msr_top_next + 1) % static_cast<u32>(msr_top_readbacks.size());
    }
}

void RendererVulkan::ConsumeMsrTopReadbacks() {
    const vk::Format format = main_present_window.GetSurfaceFormat();
    const bool bgr = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
    constexpr u32 width = Core::kScreenTopWidth, height = Core::kScreenTopHeight;
    for (auto& readback : msr_top_readbacks) {
        if (!readback.pending || !scheduler.IsFree(readback.tick)) {
            continue;
        }
        readback.pending = false;
        vmaInvalidateAllocation(instance.GetAllocator(), readback.allocation, 0, VK_WHOLE_SIZE);
        const auto* pixels = static_cast<const u8*>(readback.mapped);
        std::vector<u8> rgb(width * height * 3);
        for (u32 i = 0; i < width * height; ++i) {
            rgb[i * 3] = pixels[i * 4 + (bgr ? 2 : 0)];
            rgb[i * 3 + 1] = pixels[i * 4 + 1];
            rgb[i * 3 + 2] = pixels[i * 4 + (bgr ? 0 : 2)];
        }
        const auto path = std::filesystem::path(MSR::DrawProbe::directory) /
                          fmt::format("top-{}-f{:06}-t{}.ppm", msr_top_sequence, readback.frame,
                                      readback.emulated_us / 1000);
        if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
            std::fprintf(file, "P6\n%u %u\n255\n", width, height);
            std::fwrite(rgb.data(), 1, rgb.size(), file);
            std::fclose(file);
        }
    }
}

bool RendererVulkan::RecordMsrReadback(MsrReadback& readback, Frame& probe, u32 screen_id,
                                       u32 width, u32 height) {
    if (!readback.buffer) {
        const VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = width * height * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        const VmaAllocationCreateInfo alloc_info = {
            .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        };
        VkBuffer buffer{};
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(instance.GetAllocator(), &buffer_info, &alloc_info, &buffer,
                            &readback.allocation, &info) != VK_SUCCESS) {
            return false;
        }
        readback.buffer = buffer;
        readback.mapped = info.pMappedData;
    }
    if (!probe.framebuffer) {
        main_present_window.RecreateFrame(&probe, width, height);
    }

    const Layout::FramebufferLayout probe_layout{width, height, false, true, {},
                                                 {0, 0, width, height}, true};
    PrepareDraw(&probe, probe_layout);
    // Pipelines with static blend state (the overlay's) reset the blend constants the present
    // pipeline blends with; set them again or the probe draws nothing.
    ApplySecondLayerOpacity(1.0f);
    draw_info.modelview = MakeOrthographicMatrix(width, height);
    DrawSingleScreen(screen_id, 0.f, 0.f, static_cast<f32>(width), static_cast<f32>(height),
                     Layout::DisplayOrientation::Landscape);
    scheduler.Record([image = probe.image, buffer = readback.buffer, width,
                      height](vk::CommandBuffer cmdbuf) {
        cmdbuf.endRenderPass();
        const vk::ImageMemoryBarrier read_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, read_barrier);
        const vk::BufferImageCopy copy = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, 1},
        };
        cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer, copy);
        const vk::MemoryBarrier host_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eHost, {}, host_barrier, {}, {});
    });
    readback.tick = scheduler.CurrentTick();
    readback.emulated_us = system.CoreTiming().GetGlobalTimeUs().count();
    readback.pending = true;
    return true;
}

void RendererVulkan::ConsumeMsrReadbacks() {
    namespace P = MSR::Presentation;
    MsrReadback* newest = nullptr;
    const vk::Format surface_format = main_present_window.GetSurfaceFormat();
    const bool bgr = surface_format == vk::Format::eB8G8R8A8Unorm ||
                     surface_format == vk::Format::eB8G8R8A8Srgb;
    for (auto& readback : msr_readbacks) {
        if (readback.pending && scheduler.IsFree(readback.tick)) {
            readback.pending = false;
            if (msr_sequence_left > 0) {
                // Developer: bottomseq.trigger writes consecutive frames with their emulated
                // time (ms), to compare animation speeds between frame rates.
                --msr_sequence_left;
                vmaInvalidateAllocation(instance.GetAllocator(), readback.allocation, 0,
                                        VK_WHOLE_SIZE);
                const auto path = std::filesystem::path(MSR::DrawProbe::directory) /
                                  fmt::format("seq-{}-{:04}-t{}.ppm", msr_sequence,
                                              msr_sequence_total - 1 - msr_sequence_left,
                                              readback.emulated_us / 1000);
                MSR::BottomClassifier::WritePpm(
                    {static_cast<const u8*>(readback.mapped), bgr}, path.string());
            }
            if (!newest || readback.tick > newest->tick) {
                newest = &readback;
            }
        }
    }
    if (!newest) {
        return;
    }
    vmaInvalidateAllocation(instance.GetAllocator(), newest->allocation, 0, VK_WHOLE_SIZE);
    const vk::Format format = main_present_window.GetSurfaceFormat();
    const MSR::BottomClassifier::Image image{
        static_cast<const u8*>(newest->mapped),
        format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb};
    const auto result = MSR::BottomClassifier::Classify(image);
    const int state = static_cast<int>(result.bottom);
    msr_bottom_peak = static_cast<f32>(MSR::BottomClassifier::Peak(image)) / 255.f;
    msr_bottom_black = MSR::BottomClassifier::DarkFraction(image, 6) > .9f;
    if (result.page == P::Page::FileList || result.page == P::Page::FileOpen) {
        // Another file may be loaded: its beam slots are read afresh.
        P::beam_slot.store(-1, std::memory_order_relaxed);
        P::beam_slots.store(0, std::memory_order_relaxed);
        msr_beam_seen_us.fill(-1);
        msr_beam_empty_us.fill(-1);
    }
    if (result.bottom == P::Bottom::Hud) {
        // The slots Samus has. In Morph Ball the game greys the row out and the unlit icons fade
        // into their slots, so going by each frame the row on the game view closed up, moving
        // the Grapple Beam to the Ice Beam's place. So a slot joins once it has held an icon for
        // 0.1 s (not a flash), and leaves once it has been empty for 1 s with the row lit (a
        // quick load); greyed out, the lit slot stays as it was too.
        const s64 now = newest->emulated_us;
        // Loading a state can take emulated time back.
        const auto held = [now](s64& since) {
            if (since < 0 || since > now) {
                since = now;
            }
            return now - since;
        };
        int owned = P::beam_slots.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < msr_beam_seen_us.size(); ++i) {
            const int bit = 1 << i;
            if (result.beams.present & bit) {
                msr_beam_empty_us[i] = -1;
                if (held(msr_beam_seen_us[i]) >= 100'000) {
                    owned |= bit;
                }
            } else {
                msr_beam_seen_us[i] = -1;
                if (result.beams.greyed) {
                    msr_beam_empty_us[i] = -1;
                } else if (held(msr_beam_empty_us[i]) >= 1'000'000) {
                    owned &= ~bit;
                }
            }
        }
        P::beam_slots.store(owned, std::memory_order_relaxed);
        if (!result.beams.greyed) {
            P::beam_slot.store(result.beams.lit, std::memory_order_relaxed);
        }
        msr_beams_greyed = result.beams.greyed;
        if (result.marker.found) {
            msr_map_marker = {result.marker.x, result.marker.y};
            // While the HUD is hidden (loading, a new area) the minimap starts on her.
            if (msr_hud_alpha <= 0.f) {
                msr_map_center = msr_map_marker;
            }
        }
        // The map's backdrop along its edges (left, top, right, bottom) beside the window on
        // her, shown where the window passes them.
        const auto& [mx0, my0, mx1, my1] = MSR::BottomClassifier::MapRect;
        const int half_w = static_cast<int>(MsrMapWindowWidth) / 2;
        const int half_h = static_cast<int>(MsrMapWindowHeight) / 2;
        const int wx0 = std::clamp(static_cast<int>(msr_map_marker.x) - half_w, mx0, mx1 - 1);
        const int wx1 = std::clamp(static_cast<int>(msr_map_marker.x) + half_w, mx0 + 1, mx1);
        const int wy0 = std::clamp(static_cast<int>(msr_map_marker.y) - half_h, my0, my1 - 1);
        const int wy1 = std::clamp(static_cast<int>(msr_map_marker.y) + half_h, my0 + 1, my1);
        const std::array<std::array<int, 4>, 4> strips{{{mx0, wy0, mx0 + 2, wy1},
                                                        {wx0, my0, wx1, my0 + 2},
                                                        {mx1 - 2, wy0, mx1, wy1},
                                                        {wx0, my1 - 2, wx1, my1}}};
        for (std::size_t i = 0; i < strips.size(); ++i) {
            const auto& [x0, y0, x1, y1] = strips[i];
            const auto [r, g, b] = MSR::BottomClassifier::Backdrop(image, x0, y0, x1, y1);
            msr_map_backdrop[i] = Common::MakeVec(r / 255.f, g / 255.f, b / 255.f);
        }
    }
    if (result.bottom == P::Bottom::Prompt) {
        // The game centres each line on x 160 and types it from the left, so the box made even
        // about the centre has the line's final width from its first letter on.
        const auto box = MSR::BottomClassifier::BrightBox(image, 140);
        if (!box.Empty()) {
            const int x0 = std::min(box.x0, 320 - box.x1);
            msr_prompt_text = {static_cast<f32>(x0), static_cast<f32>(box.y0),
                               static_cast<f32>(320 - x0), static_cast<f32>(box.y1)};
        }
    }
    // A prompt stays on through dim moments (the title's prompt pulses; the prologue's pages
    // fade out and the next one starts typing), but goes at once, without fading, when the next
    // screen comes: the file select and the loading emblem fade in from black over the whole
    // screen, and drawn as the prompt, brightened as text on black is, they flashed. A prompt's
    // text never reaches the screen's edges. No prompt shows for 0.3 s after.
    if (result.bottom != P::Bottom::Prompt) {
        bool other = result.bottom == P::Bottom::Menu || result.bottom == P::Bottom::Hud;
        if (!other) {
            const auto content = MSR::BottomClassifier::BrightBox(image, 4);
            other = !content.Empty() && (content.x0 == 0 || content.y0 == 0 ||
                                         content.x1 == MSR::BottomClassifier::Width ||
                                         content.y1 == MSR::BottomClassifier::Height);
        }
        if (other) {
            msr_prompt_foreign_us = newest->emulated_us;
            msr_prompt_alpha = 0.f;
        }
    }

    // Developer calibration: <user>/msr/bottom.trigger dumps the next readback.
    if (MSR::DrawProbe::enabled.load(std::memory_order_relaxed) && MSR::DrawProbe::frame % 15 == 0 &&
        !MSR::DrawProbe::directory.empty()) {
        std::error_code ec;
        const auto trigger = std::filesystem::path(MSR::DrawProbe::directory) / "bottom.trigger";
        if (std::filesystem::exists(trigger, ec)) {
            std::filesystem::remove(trigger, ec);
            const auto path = std::filesystem::path(MSR::DrawProbe::directory) /
                              fmt::format("bottom-{}-s{}.ppm", ++msr_dumps, state);
            MSR::BottomClassifier::WritePpm(image, path.string());
        }
        // The sequence triggers may hold the number of frames to write.
        const auto take_count = [&ec](const std::filesystem::path& trigger, u32 fallback) {
            u32 count = fallback;
            if (std::FILE* file = std::fopen(trigger.string().c_str(), "r")) {
                if (std::fscanf(file, "%u", &count) != 1 || count == 0) {
                    count = fallback;
                }
                std::fclose(file);
            }
            std::filesystem::remove(trigger, ec);
            return std::min(count, 2000u);
        };
        const auto top_sequence = std::filesystem::path(MSR::DrawProbe::directory) / "topseq.trigger";
        if (std::filesystem::exists(top_sequence, ec)) {
            msr_top_left = take_count(top_sequence, 120);
            ++msr_top_sequence;
        }
        const auto sequence = std::filesystem::path(MSR::DrawProbe::directory) / "bottomseq.trigger";
        if (std::filesystem::exists(sequence, ec)) {
            msr_sequence_left = take_count(sequence, 150);
            msr_sequence_total = msr_sequence_left;
            ++msr_sequence;
        }
    }

    // Require a few agreeing frames over 80 ms of emulated time, so transitions and flashes do
    // not flicker the layout at any frame rate: as the pause menu opens and closes, the HUD
    // slides a few rows up for ~40 ms and reads as a menu, and above 60 FPS each of those
    // frames is read back several times.
    if (state == msr_candidate) {
        ++msr_candidate_frames;
    } else {
        msr_candidate = state;
        msr_candidate_frames = 1;
        msr_candidate_since_us = newest->emulated_us;
    }
    if (msr_candidate_frames >= 3 && newest->emulated_us - msr_candidate_since_us >= 80'000) {
        P::bottom.store(state, std::memory_order_relaxed);
        P::page.store(static_cast<int>(result.page), std::memory_order_relaxed);
    }
}

} // namespace Vulkan
