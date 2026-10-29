// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The texture packs the player enabled (msr/mods.h), handed to the renderer's custom texture
// manager, which loads them when the game boots. Header-only and inert until the frontend sets
// `managed`, like msr/presentation.h: until then the emulator reads its own load/textures folder.

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace MSR::TexturePacks {

/// The packs below replace the emulator's load/textures/<title> folder.
inline std::atomic<bool> managed{false};

inline std::mutex mutex;
/// Folders of the enabled packs, highest priority first, each ending in a separator.
inline std::vector<std::string> folders;

inline std::vector<std::string> Folders() {
    std::scoped_lock lock{mutex};
    return folders;
}

inline void SetFolders(std::vector<std::string> enabled) {
    std::scoped_lock lock{mutex};
    folders = std::move(enabled);
    managed = true;
}

} // namespace MSR::TexturePacks
