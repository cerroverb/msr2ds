// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/logging/log.h"
#include "common/settings.h"
#include "msr/defaults.h"

namespace MSR::Defaults {

void ApplyFirstRun() {
    auto& values = Settings::values;

    values.graphics_api = Settings::GraphicsAPI::Vulkan;
    // 9x renders the top screen at 2160 lines: 4K at 16:9.
    values.resolution_factor = 9;
    values.use_vsync = true;
    values.async_shader_compilation = true;
    values.use_disk_shader_cache = true;

    // The top screen is the game; the bottom screen carries the map and Aeion gauge. Keep it
    // visible in the corner instead of hiding it (F10 cycles layouts, F9 swaps the screens).
    values.layout_option = Settings::LayoutOption::LargeScreen;
    values.large_screen_proportion = 4.f;
    values.small_screen_position = Settings::SmallScreenPosition::BottomRight;
    values.swap_screen = false;
    values.upright_screen = false;

    LOG_INFO(Frontend, "Applied Samus Returns first-run defaults");
}

void ApplyEveryLaunch() {
    // The single-screen presentation is Vulkan only, and owns the layout and stereo output.
    Settings::values.graphics_api = Settings::GraphicsAPI::Vulkan;
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        Settings::values.layout_option = Settings::LayoutOption::LargeScreen;
    }
    Settings::values.render_3d = Settings::StereoRenderOption::Off;
}

} // namespace MSR::Defaults
