// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Compiled with the generated code: the registry as one Module. A library built from the
// player's dump (MSR_AOT_SHARED, tools/msr/recomp/build_module.py) exports it; a recompiled
// build links it in. MSR_AOT_RUNTIME_SHA256 is the hash of aot_runtime.h (see aot_generated.h).

#include "aot_generated.h"

#ifndef MSR_AOT_RUNTIME_SHA256
#error "MSR_AOT_RUNTIME_SHA256 must be defined (the SHA-256 of aot_runtime.h)"
#endif

#if defined(MSR_AOT_SHARED) && defined(_WIN32)
#define MSR_AOT_EXPORT __declspec(dllexport)
#elif defined(MSR_AOT_SHARED)
#define MSR_AOT_EXPORT __attribute__((visibility("default")))
#else
#define MSR_AOT_EXPORT
#endif

extern "C" MSR_AOT_EXPORT const MsrAot::Module* msr_aot_module() {
    // Filled on each call rather than initialized once: no guard variable, so the library
    // needs nothing from the C++ runtime.
    static MsrAot::Module module;
    module = MsrAot::Module{
        MsrAot::kModuleVersion,
        static_cast<MsrAot::u32>(sizeof(MsrAot::Ctx)),
        MSR_AOT_RUNTIME_SHA256,
        MsrAot::kShards,
        MsrAot::kShardCount,
        MsrAot::kCodeSha256,
        MsrAot::kTextBase,
        MsrAot::kTextSize,
        MsrAot::kPatchable,
        MsrAot::kPatchableCount,
    };
    return &module;
}

#ifdef MSR_AOT_SHARED
// Not part of Module, so libraries built before it still load: msr-native looks this up when a game mod changes
// the code, and asks for a rebuild without it. (A recompiled build links code generated earlier, without spans.)
extern "C" MSR_AOT_EXPORT const MsrAot::RegionSpan* msr_aot_region_spans(MsrAot::u32* count) {
    *count = MsrAot::kRegionSpanCount;
    return MsrAot::kRegionSpans;
}
#endif
