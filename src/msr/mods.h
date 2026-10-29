// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Mods: texture packs. Each folder in <user>/mods/textures/ is a pack: textures named as the
// emulator dumps them (tex1_WxH_HASH_FMT.png, .dds or .ktx, in any subfolders) and optionally a
// pack.json, as Azahar and Citra packs are made. A folder holding only folders, such as a pack's
// "Optional Addons", lists each of them as a pack. Packs that ship with the game (the textures
// folder beside it, GameFolder::BuiltInTextures) are listed the same way; a player's pack of the
// same name takes the place of one. The player enables and orders them in the launcher or the
// in-game menu; the enabled ones load when the game boots, and where packs replace the same
// texture the first one listed wins. Qt thread only.

#pragma once

#include <vector>
#include <QString>

namespace MSR::Mods {

struct TexturePack {
    QString name; ///< Folder, relative to the packs folder (Optional Addons/Updated UI)
    QString path; ///< Absolute folder path, ending in a separator
    bool enabled = true;
    bool built_in = false; ///< Ships with the game
};

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

/// Hands the enabled packs to the renderer for the next boot (msr/texture_packs.h) and turns
/// custom textures on when there are any. Returns what will load (packs and texture options),
/// to tell when a restart is needed.
QString Publish();

/// What Publish() would return now.
QString Pending();

/// Shows the texture packs folder in the file manager.
void OpenFolder();

} // namespace MSR::Mods
