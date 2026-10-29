// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Frame rate of Samus Returns. The game draws 30 frames per second: its main loop (0x156450)
// sleeps until 33 ms have passed since the previous frame, then waits for the display's VBlank.
// Everything in the game advances by the time measured between frames, so a higher rate needs
// only a shorter limit, a display refresh at least that fast, and enough CPU time to finish each
// frame; the game runs at the same speed. Shared by the frontend, core and video_core;
// header-only and inert until the frontend sets `target`, like msr/presentation.h.

#pragma once

#include <array>
#include <atomic>
#include "common/common_types.h"

namespace MSR::FrameRate {

/// The rates offered to the player.
inline constexpr std::array<int, 4> Choices{30, 60, 120, 144};

/// Frames per second chosen by the player; 0 leaves the emulator as upstream.
inline std::atomic<int> target{0};

/// Refresh rate of the monitor showing the game (Hz), 0 if unknown.
inline std::atomic<double> display_refresh{0.0};

/// Frames the game has finished (top-screen buffer swaps), for the FPS counter.
inline std::atomic<u64> game_frames{0};

/// The 3DS display: 268,111,856 Hz / 4,481,136 ticks per frame.
inline constexpr double NativeRefresh = 268111856.0 / 4481136.0;

/// The emulated display's refresh rate: the 3DS's own up to 60 FPS, the frame rate above.
constexpr double RefreshRate(int fps) {
    return fps > 60 ? static_cast<double>(fps) : NativeRefresh;
}

/// The game's frame limit in whole milliseconds. Above 30 FPS it stays just under the frame
/// period, so the VBlank wait that follows sets the pace: 16, 8 and 6 ms.
constexpr u32 LimitMs(int fps) {
    return fps <= 30 ? 33 : static_cast<u32>((1000 + fps - 1) / fps - 1);
}

/// The CPU clock (percent of the 3DS's) for a frame rate. At 30 FPS the game keeps the CPU busy
/// for about 19 of its 33 ms; faster rates scale that with a quarter to spare.
constexpr u32 ClockPercent(int fps, u32 base_percent) {
    return fps <= 30 ? base_percent : base_percent * static_cast<u32>(fps) * 125 / 3000;
}

/// Presenting faster than the monitor refreshes: frames are replaced rather than queued, so the
/// game never waits for the display.
inline bool OutpacesDisplay() {
    const int fps = target.load(std::memory_order_relaxed);
    const double display = display_refresh.load(std::memory_order_relaxed);
    return fps > 60 && (display <= 0.0 || display + 1.0 < fps);
}

/// The limit's instructions in the game (USA): `cmp r0, #33`, `bhs` past the sleep, and
/// `rsb r6, r0, #33`. The immediates are rewritten in place; the recompiled code checks these
/// words and hands them to the JIT when they differ (tools/msr/recomp/patchable.txt).
inline constexpr u32 LimitCompare = 0x156484;
inline constexpr u32 LimitBranch = 0x156488;
inline constexpr u32 LimitSubtract = 0x15648C;
inline constexpr u32 CompareBase = 0xE3500000;  // cmp r0, #imm8
inline constexpr u32 BranchWord = 0x2A000005;   // bhs 0x1564A4
inline constexpr u32 SubtractBase = 0xE2606000; // rsb r6, r0, #imm8

/// Parts of the interface (the HUD on the bottom screen among them) advance by a constant
/// 1/30 s per frame instead of the measured frame time, which at 144 FPS runs their animations
/// 4.8 times too fast. Above 30 FPS these instructions are rewritten so they take the measured
/// time; at 30 FPS that is the same 1/30 s, and the originals come back.
struct StepPatch {
    u32 address;
    u32 original;
    u32 replacement;
};
inline constexpr std::array<StepPatch, 3> StepPatches{{
    // The HUD's update (0x49DFDC) runs its timers and children with a constant:
    // vldr s16, =1/30 -> vmov.f32 s16, s0, the frame time it is given.
    {0x49DFF8, 0xED9F8AA6, 0xEEB08A40},
    // Another interface class's update (0x4E3B40, vtable 0x6E5270) swaps its frame time for the
    // constant in most of its states: vldr s16, =1/30 -> mov r0, r0, keeping the frame time.
    {0x4E3BA4, 0xED9F8AD5, 0xE1A00000},
    // The interface's fixed-step mode (game object +0x1804, step at +0x340; not seen in play):
    // vldrne s0, [r4, #0x340] -> mov r0, r0, so 0x10C688 keeps the measured time.
    {0x10C6B4, 0x1D940AD0, 0xE1A00000},
}};

} // namespace MSR::FrameRate
