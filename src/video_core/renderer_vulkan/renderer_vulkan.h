// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "common/math_util.h"
#include "video_core/renderer_base.h"
#ifdef HAVE_LIBRETRO
#include "citra_libretro/libretro_vk.h"
#else
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#endif
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Core {
class System;
}

namespace Memory {
class MemorySystem;
}

namespace Pica {
class PicaCore;
}

namespace Layout {
struct FramebufferLayout;
}

namespace VideoCore {
class GPU;
}

namespace Vulkan {

struct TextureInfo {
    u32 width;
    u32 height;
    Pica::PixelFormat format;
    vk::Image image;
    vk::ImageView image_view;
    VmaAllocation allocation;
};

struct ScreenInfo {
    TextureInfo texture;
    Common::Rectangle<f32> texcoords;
    vk::ImageView image_view;
};

struct PresentUniformData {
    std::array<f32, 4 * 4> modelview;
    Common::Vec4f i_resolution;
    Common::Vec4f o_resolution;
    int screen_id_l = 0;
    int screen_id_r = 0;
    int layer = 0;
    int reverse_interlaced = 0;
};
static_assert(sizeof(PresentUniformData) == 112,
              "PresentUniformData does not structure in shader!");

class RendererVulkan : public VideoCore::RendererBase {
    static constexpr std::size_t PRESENT_PIPELINES = 3;

public:
    explicit RendererVulkan(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window,
                            Frontend::EmuWindow* secondary_window);
    ~RendererVulkan() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return &rasterizer;
    }

    void NotifySurfaceChanged(bool second) override;

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override {}

private:
    void ReloadPipeline(Settings::StereoRenderOption render_3d);
    void CompileShaders();
    void BuildLayouts();
    void BuildPipelines();
    void ConfigureFramebufferTexture(TextureInfo& texture,
                                     const Pica::FramebufferConfig& framebuffer);
    void ConfigureRenderPipeline();
    void PrepareRendertarget();
    void RenderScreenshot();
    void RenderScreenshotWithStagingCopy();
    bool TryRenderScreenshotWithHostMemory();
    void PrepareDraw(Frame* frame, const Layout::FramebufferLayout& layout);
    void RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                        bool flipped);

    void DrawScreens(Frame* frame, const Layout::FramebufferLayout& layout, bool flipped);
    void DrawBottomScreen(const Layout::FramebufferLayout& layout,
                          const Common::Rectangle<u32>& bottom_screen);

    void DrawTopScreen(const Layout::FramebufferLayout& layout,
                       const Common::Rectangle<u32>& top_screen);
    void DrawSingleScreen(u32 screen_id, float x, float y, float w, float h,
                          Layout::DisplayOrientation orientation);
    void DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y, float w,
                                float h, Layout::DisplayOrientation orientation);

    void ApplySecondLayerOpacity(float alpha);

    void DrawCursor(const Layout::FramebufferLayout& layout);

    void LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer, ScreenInfo& screen_info,
                            bool right_eye);
    void FillScreen(Common::Vec3<u8> color, const TextureInfo& texture);

    // Samus Returns single-screen compositor (msr/presentation.h)
    void InitMsrCompositor();
    void DestroyMsrCompositor();
    void DrawMsrScreens(Frame* frame, const Layout::FramebufferLayout& layout);
    struct MsrStyle {
        f32 opacity = 1.f;
        bool keyed = false; ///< Fade the dark backdrop out by brightness
        f32 key_low = 0.f;
        f32 key_high = 0.f;
        int feather = 0; ///< Soft edge width, in percent of the piece
        int circle = 0;  ///< Circular mask radius, in percent of the piece (0 = none)
        bool over_black = false; ///< Drawn over black: brightness is coverage, see-through
        f32 gain = 1.f;          ///< Over black: brightness multiplier applied first
        bool hard_borders = false; ///< No soft edge where the piece meets its screen's border
    };
    /// Draws part of a screen; `source` is normalized with x right and y down.
    void DrawMsrRegion(u32 screen_id, const Common::Rectangle<f32>& source, f32 x, f32 y, f32 w,
                       f32 h, const MsrStyle& style);
    void DrawMsrFill(f32 x, f32 y, f32 w, f32 h, const Common::Vec4f& color, int feather = 0);
    void ReadBackMsrBottom();
    void ConsumeMsrReadbacks();

private:
    Memory::MemorySystem& memory;
    Pica::PicaCore& pica;

#ifdef HAVE_LIBRETRO
    LibRetroVKInstance instance;
#else
    Instance instance;
#endif
    Scheduler scheduler;
    RenderManager renderpass_cache;
    PresentWindow main_present_window;
    StreamBuffer vertex_buffer;
    DescriptorUpdateQueue update_queue;
    RasterizerVulkan rasterizer;
    std::unique_ptr<PresentWindow> secondary_present_window_ptr;
    DescriptorHeap present_heap;
    vk::UniquePipelineLayout present_pipeline_layout;
    std::array<vk::Pipeline, PRESENT_PIPELINES> present_pipelines;
    std::array<vk::ShaderModule, PRESENT_PIPELINES> present_shaders;
    std::array<vk::Sampler, 2> present_samplers;
    vk::ShaderModule present_vertex_shader;
    u32 current_pipeline = 0;

    std::array<ScreenInfo, 3> screen_infos{};
    PresentUniformData draw_info{};
    vk::ClearColorValue clear_color{};

    vk::ShaderModule cursor_vertex_shader{};
    vk::ShaderModule cursor_fragment_shader{};
    vk::Pipeline cursor_pipeline{};
    vk::UniquePipelineLayout cursor_pipeline_layout{};
    bool isSecondaryWindow;
    bool secondaryWindowEnabled;
    bool screenRendered;

    struct MsrReadback {
        vk::Buffer buffer{};
        VmaAllocation allocation{};
        void* mapped{};
        u64 tick{};
        s64 emulated_us{}; ///< Emulated time of the frame
        bool pending{};
    };
    vk::ShaderModule msr_shader{};
    vk::Pipeline msr_pipeline{};
    Frame msr_probe{};
    std::array<MsrReadback, 3> msr_readbacks{};
    u32 msr_readback_next = 0;
    int msr_candidate = -1;
    int msr_candidate_frames = 0;
    s64 msr_candidate_since_us = 0; ///< Emulated time the candidate was first read
    f32 msr_panel_alpha = 0.f;
    f32 msr_hud_alpha = 0.f;
    f32 msr_prompt_alpha = 0.f;
    s64 msr_last_us = 0;           ///< Emulated time of the last composite, for the fades
    long long msr_start_seen = -1; ///< Presentation::start_pressed_us already handled
    s64 msr_pausing_until_us = 0;  ///< Pause menu opening: only the menu may show until then
    s64 msr_resuming_until_us = 0; ///< Pause menu closed: no prompt until the HUD is back
    bool msr_paused = false;       ///< The pause menu Start opened is up
    s64 msr_prompt_since_us = 0;   ///< When the classifier last turned to Prompt
    s64 msr_prompt_seen_us = 0;    ///< When the classifier last saw a prompt
    /// A prompt's text on the bottom screen, as last read; the card behind it, which grows with
    /// the text and follows a smaller page once it has held (msr_prompt_card_us: since when it
    /// matches the text, 0 while no prompt shows), and the card as drawn, easing to it.
    Common::Rectangle<f32> msr_prompt_text{};
    Common::Rectangle<f32> msr_prompt_card{};
    Common::Rectangle<f32> msr_prompt_card_shown{};
    s64 msr_prompt_card_us = 0;
    s64 msr_prompt_tall_us = 0; ///< Last shown text too long for the panel's scale
    s64 msr_prompt_foreign_us = 0; ///< When another screen last showed (ConsumeMsrReadbacks)
    f32 msr_prompt_fit = 0.f;   ///< 0 the panel's scale, 1 the scale that fits nine lines
    f32 msr_bottom_peak = 0.f;     ///< Brightest pixel of the last classified bottom frame
    bool msr_bottom_black = false; ///< That frame was black apart from its text
    f32 msr_gallery = 0.f;         ///< Gallery layout (view left, panel beside it), 0..1
    /// Samus's marker on the HUD's map, as last found (bottom-screen pixels), and the middle of
    /// the minimap's window, which glides to it.
    Common::Vec2f msr_map_marker{125.f, 135.f};
    Common::Vec2f msr_map_center{125.f, 135.f};
    /// The map's backdrop along its left, top, right and bottom edges, beside the window.
    std::array<Common::Vec3f, 4> msr_map_backdrop{};
    /// The HUD's beam slots (ConsumeMsrReadbacks): since when each has read as holding an icon,
    /// and as empty with the row lit (emulated us, -1: not). Whether the game has greyed the row
    /// out (Morph Ball), and the drawn row's progress towards that look (0..1).
    std::array<s64, 3> msr_beam_seen_us{-1, -1, -1};
    std::array<s64, 3> msr_beam_empty_us{-1, -1, -1};
    bool msr_beams_greyed = false;
    f32 msr_beam_grey = 0.f;
    u32 msr_dumps = 0;
    u32 msr_sequence_left = 0; ///< Readbacks still to write for bottomseq.trigger
    u32 msr_sequence_total = 0;
    u32 msr_sequence = 0;
};

} // namespace Vulkan
