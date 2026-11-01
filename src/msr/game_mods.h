// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The game mods the player enabled (msr/mods.h), handed to the game loader
// (core/file_sys/ncch_container.cpp), which layers them over the game when it boots. Header-only
// and inert until the frontend sets `managed`, like msr/texture_packs.h: until then the emulator
// reads its own load/mods/<title> folder.

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace MSR::GameMods {

/// The mods below replace the emulator's load/mods/<title> folder for the game.
inline std::atomic<bool> managed{false};

inline std::mutex mutex;
/// The booted game's program ID: other titles (system archives) keep the emulator's folder.
inline u64 program_id = 0;
/// Folders of the enabled mods, highest priority first, each ending in a separator. Each holds
/// what an emulator's load/mods/<title> folder does: romfs/, romfs_ext/, exefs/, code.ips...
inline std::vector<std::string> folders;

/// Whether the mods apply to `title` (an update's ID counts as its game's).
inline bool Applies(u64 title) {
    std::scoped_lock lock{mutex};
    return managed && title == program_id;
}

inline std::vector<std::string> Folders() {
    std::scoped_lock lock{mutex};
    return folders;
}

inline void Set(u64 program, std::vector<std::string> enabled) {
    std::scoped_lock lock{mutex};
    program_id = program;
    folders = std::move(enabled);
    managed = true;
}

} // namespace MSR::GameMods
