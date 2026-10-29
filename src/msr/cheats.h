// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Extras that the game gives through amiibo, available without them. Each save file keeps its
// unlocks, read with one call per flag (0x1EDC10: profile, section, key, &flag, default); those
// calls are replaced by storing true, which puts Metroid II art, concept art and the music in
// Options > Gallery. Fusion Mode is a difficulty of a save's New Game, a list the game shows
// once that save has beaten the game; the menus' checks are replaced so the list (Normal, Hard,
// Fusion) always comes up. The Fusion suit follows one flag of the game object (+0x1859, set
// from the save's difficulty), read separately by each thing that draws Samus; those reads are
// replaced by true, apart from the one that raises the damage Samus takes, so the suit leaves
// the difficulty as chosen; the models already built then switch too (msr/suit_swap.h).
// Nothing is written to the saves by these patches.
// Shared by the frontend and video_core; header-only and inert until the frontend sets `enabled`.

#pragma once

#include <array>
#include <atomic>
#include "common/common_types.h"

namespace MSR::Cheats {

enum Flag : u32 {
    Galleries = 1 << 0,  ///< Concept art, Metroid II art and the sound test
    FusionMode = 1 << 1, ///< Fusion Mode offered with the other difficulties
    FusionSuit = 1 << 2, ///< Samus in the Fusion suit at every difficulty
    InfiniteEnergy = 1 << 3,   ///< Energy kept full
    InfiniteMissiles = 1 << 4, ///< Missiles, Super Missiles and Power Bombs kept full
    InfiniteAeion = 1 << 5,    ///< Aeion kept full
};

/// The flags chosen by the player.
inline std::atomic<u32> enabled{0};

/// The instructions rewritten in the game (USA) while a flag is on; the originals come back when
/// it is off. The recompiled code checks these words and hands them to the JIT when they differ
/// (tools/msr/recomp/patchable.txt).
struct Patch {
    Flag flag;
    u32 address;
    u32 original;
    u32 replacement;
};

inline constexpr u32 MovR0One = 0xE3A00001;  // mov r0, #1
inline constexpr u32 MovR1One = 0xE3A01001;  // mov r1, #1
inline constexpr u32 StoreR0 = 0xE5C30000;   // strb r0, [r3]: the call's flag

inline constexpr std::array<Patch, 26> Patches{{
    // The profile summary of a save (0x3F8xxx): ldr r0, [sp, #0x88]; bl 0x1EDC10 for
    // FusionMode, GalleryMetroidIIUnlocked, GalleryConceptArtUnlocked and GalleryMusicUnlocked.
    {FusionMode, 0x3F8D40, 0xE59D0088, MovR0One},
    {FusionMode, 0x3F8D44, 0xEBF7D3B1, StoreR0},
    {Galleries, 0x3F8E10, 0xE59D0088, MovR0One},
    {Galleries, 0x3F8E14, 0xEBF7D37D, StoreR0},
    {Galleries, 0x3F8E38, 0xE59D0088, MovR0One},
    {Galleries, 0x3F8E3C, 0xEBF7D373, StoreR0},
    {Galleries, 0x3F8E60, 0xE59D0088, MovR0One},
    {Galleries, 0x3F8E64, 0xEBF7D369, StoreR0},
    // The menus' checks of the save's Fusion Mode flag (+0x6C of the slot's summary), so every
    // save offers it: the difficulty list's entry and the two handlers that start it.
    {FusionMode, 0x4D828C, 0xE5D0006C, MovR0One},
    {FusionMode, 0x4DD3F4, 0xE5D0006C, MovR0One},
    {FusionMode, 0x4E2134, 0xE5D1106C, MovR1One},
    // New Game goes to the difficulty list once the game is beaten (+0x68), else straight to
    // Normal: ldrb r0, [r0, #0x68] -> mov r0, #1.
    {FusionMode, 0x4E3430, 0xE5D00068, MovR0One},
    // The galleries' own screens: str r4, [sp] (the default); bl 0x1EDC10, for the sound test,
    // Metroid II art and concept art.
    {Galleries, 0x492504, 0xE58D4000, MovR0One},
    {Galleries, 0x492508, 0xEBF56DC0, StoreR0},
    {Galleries, 0x495BC8, 0xE58D4000, MovR0One},
    {Galleries, 0x495BCC, 0xEBF5600F, StoreR0},
    {Galleries, 0x495D54, 0xE58D4000, MovR0One},
    {Galleries, 0x495D58, 0xEBF55FAC, StoreR0},
    // The Fusion mode flag, ldrb rN, [game, #0x1859] -> mov rN, #1, where it picks Samus's
    // resource packages (a save starting, an area loading, the suit packages)...
    {FusionSuit, 0x10D438, 0xE5D01059, MovR1One},
    {FusionSuit, 0x2707A0, 0xE5D01059, MovR1One},
    {FusionSuit, 0x143EF8, 0xE7D00006, MovR0One},
    // ...her models (actors/characters/samusfusion) and the scripts' LoadModelByFusion...
    {FusionSuit, 0x404080, 0xE5D01059, MovR1One},
    {FusionSuit, 0x3A83D4, 0xE5D00059, MovR0One},
    // ...and the interface's suit sprites (the HUD; Samus_GravitySuit in the pause menu).
    {FusionSuit, 0x250448, 0xE5D00059, MovR0One},
    {FusionSuit, 0x453468, 0xE5D00059, MovR0One},
    {FusionSuit, 0x473544, 0xE5D00059, MovR0One},
    // Left alone: the damage multiplier (0x384CBC) and the save's difficulty (0x130454, 0x263460).
}};

// The refills (video_core/gpu.cpp), each frame. Energy and Aeion live in two of Samus's
// components, found on the heap by their vtables: their current value is set to the maximum
// through the component's own setter (which also updates the inventory's copy and tells the
// HUD; the suit swap stub calls it on the game thread), then locked, as the game locks it
// itself ("current life locked", "special energy locked"), so a hit or an ability takes
// nothing; the lock is released when the cheat is turned off. Missiles live in her inventory,
// where the HUD reads them each frame. The inventory hangs from her entity (+0x18C), found as
// the game does
// (0x27109C) through the game object's player handles (+0xFC, else +0xF0, else +0xF4; each
// handle's entity at +8). It is a hash table: buckets at +0x40, its size at +0x44, their
// number at +0x48; a node holds the key's hash, the value's address (a float) at +4 and the
// next node at +0x10 (0x1AAC10). A key is a global pointing at a name whose hash is at +0x10.
inline constexpr u32 GamePointer = 0x76F314;
inline constexpr std::array<u32, 3> PlayerHandles = {0xFC, 0xF0, 0xF4};
inline constexpr u32 EntityInventoryOffset = 0x18C;

struct Refill {
    Flag flag;
    u32 current; ///< Key global of the item kept full
    u32 maximum; ///< Key global of its maximum
};

inline constexpr std::array<Refill, 3> Refills = {{
    {InfiniteMissiles, 0x70871C, 0x708720}, // ITEM_WEAPON_MISSILE_CURRENT, _MAX
    {InfiniteMissiles, 0x708734, 0x708738}, // ITEM_WEAPON_SUPER_MISSILE_CURRENT, _MAX
    {InfiniteMissiles, 0x708728, 0x70872C}, // ITEM_WEAPON_POWER_BOMB_CURRENT, _MAX
}};

/// A component value kept at its maximum and locked: the component's vtable and size, the
/// offsets of the maximum, the current value (floats) and the lock (a byte), and the setter
/// (object in r0, value in s0) the game's reflection uses for the current value.
struct LockedValue {
    Flag flag;
    u32 vtable;
    u32 size;
    u32 maximum;
    u32 current;
    u32 lock;
    u32 setter;
};

inline constexpr std::array<LockedValue, 2> LockedValues = {{
    // CPlayerLifeComponent (created at 0x382AFC): m_fMaxLife, m_fCurrentLife, its lock
    {InfiniteEnergy, 0x6D4F98, 0xF8, 0x34, 0x38, 0x40, 0x5E5630},
    // CSpecialEnergyComponent (created at 0x3BC0D4): fMaxEnergy, its energy, its lock
    {InfiniteAeion, 0x6D8AF0, 0x3C, 0x30, 0x34, 0x38, 0x5E68C4},
}};

} // namespace MSR::Cheats
