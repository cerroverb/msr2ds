// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The recompiled code's registry, defined by the generated aot_registry.cpp
// (tools/msr/recomp/generate.py): every resume point and its function, by shard, the hash of
// the text segment the code was generated from, and the instructions that may be rewritten in
// memory at run time (patchable.txt) with their original encodings.

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

} // namespace MsrAot
