// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Mods: game mods and texture packs. The player enables and orders them in the launcher or the
// in-game menu; the enabled ones load when the game boots, and where two change the same thing
// the first one listed wins. Qt thread only.
//
// Game mods: each folder in <user>/mods/game/ is a mod, laid out as 3DS emulators (Citra,
// Azahar) and Luma3DS take them: romfs/ (files replacing or adding to the game's), romfs_ext/
// (.ips/.bps patches of the game's files, .stub files removing them), exefs/ or the top folder
// (code.bin, code.ips or code.bps for the game's code, banner.bnr, icon.bin) and exheader.bin. A
// mod packed inside its title folder (00040000001BB200/ or luma/titles/00040000001BB200/) is
// found there. On Windows, Azahar's own mods folder for the game, where Randovania exports a
// randomizer for Azahar by default, is listed too ("Azahar's mods folder"), off at first. A
// Randovania export's patcher.json names its seed.
//
// Texture packs: each folder in <user>/mods/textures/ is a pack: textures named as the emulator
// dumps them (tex1_WxH_HASH_FMT.png, .dds or .ktx, in any subfolders) and optionally a
// pack.json, as Azahar and Citra packs are made. A folder holding only folders, such as a pack's
// "Optional Addons", lists each of them as a pack. Packs that ship with the game (the textures
// folder beside it, GameFolder::BuiltInTextures) are listed the same way; a player's pack of the
// same name takes the place of one.

#pragma once

#include <vector>
#include <QString>
#include "common/common_types.h"

namespace MSR::Mods {

struct GameMod {
    QString name; ///< Folder in the game mods folder
    QString path; ///< Its files' folder, ending in a separator; empty when it holds none
    bool enabled = true;
};

struct TexturePack {
    QString name; ///< Folder, relative to the packs folder (Optional Addons/Updated UI)
    QString path; ///< Absolute folder path, ending in a separator
    bool enabled = true;
    bool built_in = false; ///< Ships with the game
};

/// <user>/mods/game/, created if missing.
QString GameModFolder();

/// The mods on disk in the player's order. New ones come first, enabled.
std::vector<GameMod> GameMods();

/// What a mod changes (files, file patches, code), for the menus.
QString Describe(const GameMod& mod);

/// A Randovania randomizer (its romfs has actors/items/randomizerpowerup). With one, the Fusion
/// suit setting stays off (MSR::Cheats::randomizer).
bool IsRandomizer(const GameMod& mod);
/// A randomizer is among the enabled mods.
bool RandomizerEnabled();

void SetModEnabled(const QString& name, bool enabled);

/// Moves a mod up (-1, higher priority) or down (+1) the list.
void MoveMod(const QString& name, int direction);

/// Shows the game mods folder in the file manager.
void OpenModFolder();

/// <user>/mods/textures/, created if missing.
QString TexturePackFolder();

/// The packs on disk in the player's order. New ones come first, enabled, smallest first so
/// add-ons override the packs they extend.
std::vector<TexturePack> TexturePacks();

/// A pack's texture count and pack.json details, for the menus.
QString Describe(const TexturePack& pack);

void SetEnabled(const QString& name, bool enabled);

/// Moves a pack up (-1, higher priority) or down (+1) the list.
void Move(const QString& name, int direction);

/// Hands the enabled mods to the game loader (msr/game_mods.h) and the enabled packs to the
/// renderer (msr/texture_packs.h) for the game booting, `program_id`, turns custom textures on
/// when there are any and tells the emulator whether a randomizer loads. Returns what will load
/// (mods, packs and texture options), to tell when a restart is needed.
QString Publish(u64 program_id);

/// What Publish() would return now.
QString Pending();

/// Shows the texture packs folder in the file manager.
void OpenFolder();

} // namespace MSR::Mods
