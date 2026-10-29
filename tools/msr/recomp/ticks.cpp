// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Cycle costs of instructions, from the emulator's own ARM11 timing model
// (src/core/arm/dynarmic/arm_tick_counts.cpp), so the recompiled code advances emulated time
// exactly as the JIT does. generate.py builds and runs it.
//
// Input (binary, little-endian): pairs of u32 {thumb, instruction}; a Thumb BL/BLX pair has the
// first halfword in the upper 16 bits. Output: one u32 cost per pair.

#include <cstdint>
#include <cstdio>
#include <vector>
#include "core/arm/dynarmic/arm_tick_counts.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: ticks <input> <output>\n");
        return 2;
    }
    std::FILE* in = std::fopen(argv[1], "rb");
    std::FILE* out = std::fopen(argv[2], "wb");
    if (!in || !out) {
        return 1;
    }
    std::uint32_t pair[2];
    while (std::fread(pair, sizeof(pair), 1, in) == 1) {
        const auto cost = static_cast<std::uint32_t>(Core::TicksForInstruction(pair[0] != 0, pair[1]));
        std::fwrite(&cost, sizeof(cost), 1, out);
    }
    std::fclose(in);
    std::fclose(out);
    return 0;
}
