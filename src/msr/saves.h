// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Save import. Samus Returns keeps its three files in one save archive: profile0, profile1 and
// profile2 (files A, B and C), each holding samus.bmssv, common.bmssv and pkprfl.bmssv. Azahar
// stores the archive at <user>/sdmc/Nintendo 3DS/<0...>/<0...>/title/<high>/<low>/data/00000001/
// beside a 00000001.metadata file with its format. Checkpoint and JKSM back saves up as the same
// files, and Citra, Lime3DS and the other Azahar builds keep the same layout, so an import finds
// the folder holding profileN folders under whatever folder the player picks and copies it in
// place of the saves here, which go to <user>/msr/save-backups/ first. Qt thread only, while the
// game is not running.

#pragma once

#include <vector>
#include <QDateTime>
#include <QString>
#include "common/common_types.h"

namespace MSR::Saves {

/// Saves found for an import.
struct Found {
    QString folder;      ///< Holds the profileN folders
    QString metadata;    ///< The emulator's format file beside it (folder.metadata), if any
    int files = 0;       ///< Save files in it (profileN folders with the game's files)
    QDateTime modified;  ///< The newest file
    bool this_game = false; ///< Under the title folder of the game being played
};

/// The save archive's folder for this game (it may not exist yet).
QString Folder(u64 program_id);

/// Samus Returns saves under `path`, any depth (up to 12 folders): this game's title first, then
/// the newest. Never the saves already here.
std::vector<Found> Find(const QString& path, u64 program_id);

/// Replaces this game's saves with `found`. The current ones go to <user>/msr/save-backups/
/// <date-time>/ first (`backup` receives that folder, empty when there were none). Returns an
/// error message, empty on success; on failure the saves are left as they were.
QString Import(const Found& found, u64 program_id, QString* backup);

/// Opens the save folder (or the nearest existing parent) in the file manager.
void OpenFolder(u64 program_id);

} // namespace MSR::Saves
