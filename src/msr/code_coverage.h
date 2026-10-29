// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Developer code coverage, for the recompilation feasibility study (tools/msr/recomp): records
// every guest instruction word the CPU JIT translates, i.e. the code the game really runs. Inert
// unless the frontend enables it (MSR_DEV=1); then creating <user>/msr/coverage.trigger writes
// <user>/msr/coverage.bin. Record() runs on the CPU thread, Dump() on the Qt thread.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace MSR::CodeCoverage {

/// The application's code, rodata and data (0x00100000 onwards), one flag per 32-bit word.
inline constexpr std::uint32_t Base = 0x00100000;
inline constexpr std::uint32_t Size = 0x00700000;

inline std::atomic<bool> enabled{false};
inline std::unique_ptr<std::atomic<std::uint8_t>[]> words;
inline std::mutex outside_mutex;
inline std::set<std::uint32_t> outside_pages; ///< Code run elsewhere (loaded modules), by page

inline void Enable() {
    words = std::make_unique<std::atomic<std::uint8_t>[]>(Size / 4);
    enabled = true;
}

inline void Record(std::uint32_t address) {
    if (!enabled.load(std::memory_order_relaxed)) {
        return;
    }
    if (address - Base < Size) {
        words[(address - Base) / 4].store(1, std::memory_order_relaxed);
    } else {
        std::scoped_lock lock{outside_mutex};
        outside_pages.insert(address & ~0xFFFu);
    }
}

/// File: "MSRCOV1\0", base, word count, one byte per word, then the outside pages
/// (count, addresses), all little-endian.
inline bool Dump(const std::string& path) {
    if (!enabled) {
        return false;
    }
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    const std::uint32_t header[2] = {Base, Size / 4};
    std::fwrite("MSRCOV1", 1, 8, file);
    std::fwrite(header, sizeof(header), 1, file);
    for (std::uint32_t i = 0; i < Size / 4; ++i) {
        std::fputc(words[i].load(std::memory_order_relaxed), file);
    }
    std::scoped_lock lock{outside_mutex};
    const auto count = static_cast<std::uint32_t>(outside_pages.size());
    std::fwrite(&count, sizeof(count), 1, file);
    for (const std::uint32_t page : outside_pages) {
        std::fwrite(&page, sizeof(page), 1, file);
    }
    std::fclose(file);
    return true;
}

} // namespace MSR::CodeCoverage
