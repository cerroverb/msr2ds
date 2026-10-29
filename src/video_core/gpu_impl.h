// Copyright 2023 Citra Emulator Project
// Copyright 2024 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <utility>
#include <vector>
#include "common/archives.h"
#include "common/microprofile.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "video_core/gpu_debugger.h"
#include "video_core/gpu_impl.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_software/sw_blitter.h"
#include "video_core/right_eye_disabler.h"
#include "video_core/video_core.h"

namespace VideoCore {
struct GPU::Impl {
    Core::Timing& timing;
    Core::System& system;
    Memory::MemorySystem& memory;
    std::shared_ptr<Pica::DebugContext> debug_context;
    Pica::PicaCore pica;
    GraphicsDebugger gpu_debugger;
    std::unique_ptr<RendererBase> renderer;
    RasterizerInterface* rasterizer;
    std::unique_ptr<SwRenderer::SwBlitter> sw_blitter;
    Core::TimingEventType* vblank_event;
    Service::GSP::InterruptHandler signal_interrupt;

    /// Samus Returns: the Fusion suit switched in game (GPU::ApplyMsrSuitSwap).
    struct MsrSuitSwap {
        enum class Step { Idle, Loading, Settling, Swapping, Releasing, Aborting, Calling };
        Step step = Step::Idle;
        bool target = false;    ///< The family being switched to
        int given_up = -1;      ///< The family a swap failed to reach (-1: none)
        u32 runs = 0;           ///< The stub's count before the command out
        s64 since_us = 0;       ///< When the command went out, or the wait started
        s64 next_scan_us = 0;   ///< When to look for the components again
        std::vector<u32> components;
        /// Models hanging from Samus's bones before the swap: scene node and bone name hash.
        std::vector<std::pair<u32, u32>> attachments;
        /// Calls the stub makes when it is free (the refills' setters): function, r0, r1, s0.
        std::vector<std::array<u32, 4>> calls;
    } msr_suit_swap;

    /// Samus Returns: the components the cheats keep full (GPU::ApplyMsrRefills), by
    /// MSR::Cheats::LockedValues, and whether the cheat locked them.
    struct MsrRefills {
        std::array<u32, 2> components{};
        std::array<bool, 2> locked{};
        s64 next_scan_us = 0;
    } msr_refills;

    explicit Impl(Core::System& system, Frontend::EmuWindow& emu_window,
                  Frontend::EmuWindow* secondary_window)
        : timing{system.CoreTiming()}, system{system}, memory{system.Memory()},
          debug_context{Pica::g_debug_context}, pica{memory, debug_context},
          renderer{VideoCore::CreateRenderer(emu_window, secondary_window, pica, system)},
          rasterizer{renderer->Rasterizer()},
          sw_blitter{std::make_unique<SwRenderer::SwBlitter>(memory, rasterizer)} {}
    ~Impl() = default;
};
} // namespace VideoCore