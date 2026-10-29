// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace MSR::Defaults {

/// Core settings for a fresh install: Vulkan, window-sized internal resolution, a large top screen
/// with the map screen beside it, and stutter-free shader compilation. Only applied on first run,
/// so anything changed later in Settings (F1) is kept.
void ApplyFirstRun();

/// Settings that must hold every launch for the single-game player to work.
void ApplyEveryLaunch();

} // namespace MSR::Defaults
