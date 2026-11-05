// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The recompiled code's registry, defined by the generated aot_registry.cpp
// (tools/msr/recomp/generate.py): every resume point and its function, by shard, the hash of
// the text segment the code was generated from, and the instructions that may be rewritten in
// memory at run time (patchable.txt) with their original encodings.
//
// The registry reaches the emulator as a Module (aot_module.cpp): linked into the program by a
// recompiled build (ENABLE_MSR_RECOMP), or in a library the player builds from their own dump
// (tools/msr/recomp/build_module.py), which the emulator loads at boot.

#pragma once

#include "aot_runtime.h"

namespace MsrAot {

struct Shard {
    const Entry* entries;
    const u32* count;
};

extern const Shard kShards[];
extern const u32 kShardCount;
extern const u8 kCodeSha256[32];
extern const u32 kTextBase;
extern const u32 kTextSize;

struct Patchable {
    u32 address;
    u32 size; ///< 2 or 4 bytes
    u32 original;
};
extern const Patchable kPatchable[];
extern const u32 kPatchableCount;

/// A run of one recompiled function's code ([lo, hi)), by its entry (a function may have several). Code patched
/// by a game mod runs on an interpreter in msr-native, which turns off the functions covering it
/// (msr_aot_region_spans).
struct RegionSpan {
    u32 entry;
    u32 thumb;
    u32 lo;
    u32 hi;
};
extern const RegionSpan kRegionSpans[];
extern const u32 kRegionSpanCount;

/// Changes when Module's layout changes.
inline constexpr u32 kModuleVersion = 1;

/// What `msr_aot_module()` returns. The program uses a library only when `version`,
/// `ctx_size` and `runtime_sha256` match its own: the code was compiled against the same
/// aot_runtime.h (its SHA-256 with carriage returns removed, in hex).
struct Module {
    u32 version;
    u32 ctx_size;
    char runtime_sha256[65];
    const Shard* shards;
    u32 shard_count;
    const u8* code_sha256;
    u32 text_base;
    u32 text_size;
    const Patchable* patchable;
    u32 patchable_count;
};

} // namespace MsrAot

/// The library's one export.
extern "C" const MsrAot::Module* msr_aot_module();
