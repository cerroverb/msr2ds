// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <QString>
#include "common/common_types.h"

// Single-game player for Metroid: Samus Returns. Modeled on DekUI's MM3D::GameFolder: the player
// lives in one folder with a portable user/ directory, and only the supported game boots.
namespace MSR::GameFolder {

/// Product codes are CTR-P-A9A + region letter (E = USA, P = EUR, J = JPN).
inline constexpr char ProductCodePrefix[] = "CTR-P-A9A";
inline constexpr u64 UsaProgramId = 0x00040000001BB200;

struct RomInfo {
    QString path;
    QString error; ///< Empty when the file is a usable Samus Returns dump
    u64 program_id = 0;
    QString product_code;
    bool encrypted = false;

    bool Ok() const {
        return error.isEmpty();
    }
    QString Region() const;
};

/// Folder holding the executable (on macOS, the one holding the app bundle); portable data and
/// an optional game dump live beside it.
QString Root();
/// Portable Azahar user directory (Root()/user/, with a trailing separator). An AppImage, or a
/// macOS app, keeps it beside the AppImage file or the app if a user/ folder is there, else in
/// ~/.local/share or ~/Library/Application Support.
QString User();

/// Texture packs that ship with the game: Root()/textures/, or usr/share/samus-returns-recomp/
/// textures/ in the AppImage. Empty when there are none.
QString BuiltInTextures();
/// Shader caches that ship with the game (msr/shader_seed.h): Root()/shaders/, or the same place
/// under share/ as the textures. Empty when there are none.
QString BuiltInShaders();

/// Creates the user directory and points Azahar at it. Call before any config is loaded.
void Prepare();
/// True when no configuration existed before Prepare(): Samus Returns defaults should be applied.
bool IsFirstRun();

/// Reads the NCSD/NCCH headers only, so probing a 1 GiB dump is instant.
RomInfo Probe(const QString& path);

/// Finds the game: a dump beside the app, then in Root()/game/, then the remembered path.
QString FindRom();
/// Remembers a dump chosen outside the application folder.
void RememberRom(const QString& path);

/// The dump this session boots, chosen before the main window exists.
void SetActiveRom(const QString& path);
QString ActiveRom();

} // namespace MSR::GameFolder
