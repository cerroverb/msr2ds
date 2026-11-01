// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The Fusion suit switched while Samus Returns (USA) runs. The game picks Samus's models
// (actors/characters/samus or samusfusion) and her suit packages (packs/players/*.pkg or
// *_fusion.pkg) only while an area loads, both by one flag, and keeps one family loaded. So a
// stub placed after the end of the code (suit_swap_stub.s) does on the game thread what a load
// would: it loads the other family's packages, empties each of Samus's model components (Samus
// and the morph ball) as their destructor does, registers the models again and puts the one on
// screen back, then releases the first family. The emulator (video_core/gpu.cpp) hands it one
// command per frame through a mailbox, branching to it from the frame limiter only while a
// command waits. Shared by the frontend and video_core; inert unless the game is found.

#pragma once

#include "common/common_types.h"
#include "msr/suit_swap_stub.h"

namespace MSR::SuitSwap {

/// The stub and its mailbox, in the zeros between the end of the code (0x6C4848) and the
/// read-only data (0x6C5000), past the loader Randovania's randomizer (open-samus-returns-rando)
/// adds at the end of the code (0x6C4850-0x6C49E8). tools/msr/suit_swap/assemble.py links the
/// stub at this address; it is written only over zeros or itself.
inline constexpr u32 StubAddress = 0x6C4A00;
inline constexpr u32 Mailbox = 0x6C4F00;

/// The frame limiter's first instruction (push {r4, r5, r6, lr}), once per frame on the game
/// thread; replaced by a branch to the stub, which replays it. Listed in patchable.txt.
inline constexpr u32 Hook = 0x156450;
inline constexpr u32 HookOriginal = 0xE92D4070;
inline constexpr u32 HookBranch = 0xEA000000u | (((StubAddress - Hook - 8) >> 2) & 0xFFFFFFu);

/// The game object: the save's Fusion Mode and the developers' ToggleForceFusionMode flag, both
/// read with the Fusion suit setting (msr/cheats.h) to pick the models.
inline constexpr u32 GamePointer = 0x76F314;
inline constexpr u32 FusionModeOffset = 0x1859;
inline constexpr u32 ForceFusionOffset = 0x185A;

/// Samus's model component (also the morph ball's): its vtable, its models (32-byte entries,
/// the file's id at +0x14, pointing at its name), their count and capacity.
inline constexpr u32 ComponentVtable = 0x6DC880;
inline constexpr u32 ComponentEntriesOffset = 0x2E4;
inline constexpr u32 EntryPathOffset = 0x14;
inline constexpr u32 ComponentCountOffset = 0x2EC;
inline constexpr u32 ComponentCapacityOffset = 0x2F0;
inline constexpr u32 ComponentSize = 0x320;

/// Model instances (the component's at +0x30) form a scene graph: each one's node at +0xF4 has
/// its parent at +0, first child at +4, next sibling at +0xC and the parent's bone it hangs from
/// at +0xB4, an index into the parent's skeleton (instance +0xF0, its bones at [[+0x44]+0x3C]: a
/// count, then 0x74-byte bones whose name id at +0x54 has its hash at +0x10). The aim laser and
/// the muzzle flash hang from Samus's cannon this way; the Fusion skeleton orders its bones
/// differently, so after a swap the emulator moves them to the bone with the same name.
inline constexpr u32 ComponentInstanceOffset = 0x30;
inline constexpr u32 InstanceSkeletonOffset = 0xF0;
inline constexpr u32 InstanceNodeOffset = 0xF4;
inline constexpr u32 NodeFirstChildOffset = 0x4;
inline constexpr u32 NodeNextOffset = 0xC;
inline constexpr u32 NodeBoneOffset = 0xB4;
inline constexpr u32 BoneSize = 0x74;
inline constexpr u32 BoneNameOffset = 0x54;

enum Command : u32 {
    LoadFamily = 1,    ///< Load the family's packages
    SwapModels = 2,    ///< Register the components' models again and replace the one shown
    ReleaseFamily = 3, ///< Release the family's packages
    CallList = 4,      ///< Call functions with an object and a float (the cheats' setters)
};

/// Mailbox fields (offsets).
inline constexpr u32 Armed = 0x00;      ///< 1: run the command on the next frame
inline constexpr u32 CommandField = 0x04;
inline constexpr u32 Family = 0x08;     ///< 0 the suits, 1 the Fusion suits
inline constexpr u32 Count = 0x0C;
inline constexpr u32 Components = 0x10; ///< Up to MaxComponents
inline constexpr u32 Runs = 0x30;       ///< Commands the stub has run
inline constexpr u32 MaxComponents = 8;
inline constexpr u32 CallCount = 0x40;
inline constexpr u32 Calls = 0x44; ///< 16 bytes each: function, r0, r1, s0
inline constexpr u32 MaxCalls = 4;

} // namespace MSR::SuitSwap
