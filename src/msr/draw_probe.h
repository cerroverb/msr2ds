// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Developer draw logger, used to derive the Samus Returns draw signatures (which draws are the
// world, the HUD, fades, the bottom-screen map...). Inert unless the frontend enables it
// (MSR_DEV=1); then creating <user>/msr/drawlog.trigger logs the next frames to
// <user>/msr/drawlog-N.txt. Renderer thread only, apart from the setup fields.

#pragma once

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace MSR::DrawProbe {

inline std::atomic<bool> enabled{false};
inline std::string directory; ///< Set once by the frontend before emulation starts.

inline unsigned frame = 0;
inline int frames_left = 0;
inline unsigned capture = 0;
inline std::FILE* file = nullptr;

inline bool Active() {
    return file != nullptr;
}

inline void Write(std::string_view line) {
    if (file) {
        std::fwrite(line.data(), 1, line.size(), file);
        std::fputc('\n', file);
    }
}

/// Called once per presented guest frame.
inline void NextFrame() {
    ++frame;
    if (file) {
        if (--frames_left <= 0) {
            std::fclose(file);
            file = nullptr;
        } else {
            std::fprintf(file, "--- frame %u\n", frame);
        }
        return;
    }
    if (!enabled.load(std::memory_order_relaxed) || frame % 30 != 0 || directory.empty()) {
        return;
    }
    std::error_code ec;
    const auto trigger = std::filesystem::path(directory) / "drawlog.trigger";
    if (!std::filesystem::exists(trigger, ec)) {
        return;
    }
    // The file may hold the number of frames to log.
    int count = 3;
    if (std::FILE* in = std::fopen(trigger.string().c_str(), "r")) {
        if (std::fscanf(in, "%d", &count) != 1 || count <= 0) {
            count = 3;
        }
        std::fclose(in);
    }
    std::filesystem::remove(trigger, ec);
    const auto path = std::filesystem::path(directory) / ("drawlog-" + std::to_string(++capture) + ".txt");
    file = std::fopen(path.string().c_str(), "w");
    frames_left = count < 2000 ? count : 2000;
    if (file) {
        std::fprintf(file, "--- frame %u\n", frame);
    }
}

} // namespace MSR::DrawProbe
