// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Shaders that ship with the game (the personal package's shaders/ folder): Azahar transferable
// shader caches holding configs and programs, without SPIR-V. When the game boots, entries the
// player's cache lacks are added to it (vk_shader_disk_cache.cpp), so their pipelines build at
// boot instead of when an effect first appears. Header-only and inert until the frontend sets a
// folder, like msr/texture_packs.h.

#pragma once

#include <mutex>
#include <string>

namespace MSR::ShaderSeed {

inline std::mutex mutex;
/// Folder of the seed caches, named like the cache files, ending in a separator; empty: none.
inline std::string folder;

inline std::string Folder() {
    std::scoped_lock lock{mutex};
    return folder;
}

inline void SetFolder(std::string path) {
    std::scoped_lock lock{mutex};
    folder = std::move(path);
}

} // namespace MSR::ShaderSeed
