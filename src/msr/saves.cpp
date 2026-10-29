// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <utility>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include "common/file_util.h"
#include "msr/game_folder.h"
#include "msr/saves.h"

namespace MSR::Saves {
namespace {

/// Folders looked through at most, so picking a whole drive cannot hang the launcher.
constexpr int MaxFolders = 20000;
constexpr int MaxDepth = 12;

/// Azahar's format record for the archive (ArchiveFormatInfo), as the game formats it: 256 KiB,
/// 8 folders, 64 files, duplicated data. Written when an import brings none.
constexpr std::array<u8, 16> DefaultMetadata = {0x00, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00, 0x00,
                                                0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};

bool IsProfile(const QString& name) {
    return name.size() == 8 && name.startsWith(QStringLiteral("profile"), Qt::CaseInsensitive) &&
           name[7].isDigit();
}

/// Copies a folder's files and subfolders into `to` (created). Returns what failed, if anything.
QString CopyTree(const QString& from, const QString& to) {
    if (!QDir().mkpath(to)) {
        return QStringLiteral("could not create %1").arg(QDir::toNativeSeparators(to));
    }
    const QDir source(from);
    for (const auto& entry :
         source.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString target = QDir(to).filePath(entry.fileName());
        if (entry.isDir()) {
            if (QString error = CopyTree(entry.absoluteFilePath(), target); !error.isEmpty()) {
                return error;
            }
            continue;
        }
        QFile file(entry.absoluteFilePath());
        if (!file.copy(target)) {
            return QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(entry.absoluteFilePath()),
                                                file.errorString());
        }
    }
    return {};
}

bool SamePath(const QString& a, const QString& b) {
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

/// The save in `folder`: its profileN folders with the game's files (`files` is 0 if none).
Found SaveAt(const QString& folder, const QFileInfoList& children) {
    Found save{.folder = folder};
    for (const auto& child : children) {
        if (!IsProfile(child.fileName())) {
            continue;
        }
        bool any = false;
        for (const auto& file : QDir(child.absoluteFilePath()).entryInfoList(QDir::Files)) {
            if (file.suffix().compare(QStringLiteral("bmssv"), Qt::CaseInsensitive) == 0) {
                any = true;
                save.modified = std::max(save.modified, file.lastModified());
            }
        }
        save.files += any;
    }
    const QFileInfo metadata(folder + QStringLiteral(".metadata"));
    if (save.files > 0 && metadata.isFile() && metadata.size() == DefaultMetadata.size()) {
        save.metadata = metadata.absoluteFilePath();
    }
    return save;
}

QFileInfoList Subfolders(const QString& folder) {
    return QDir(folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
}

} // namespace

QString Folder(u64 program_id) {
    const QString sdmc =
        QString::fromStdString(FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir));
    return QDir::cleanPath(
        sdmc + QStringLiteral("/Nintendo 3DS/00000000000000000000000000000000/"
                              "00000000000000000000000000000000/title/%1/%2/data/00000001")
                   .arg(static_cast<u32>(program_id >> 32), 8, 16, QLatin1Char('0'))
                   .arg(static_cast<u32>(program_id), 8, 16, QLatin1Char('0')));
}

std::vector<Found> Find(const QString& path, u64 program_id) {
    const QString own = Folder(program_id);
    // The title's folder in an emulator's layout (001bb200), or its unique id in Checkpoint's
    // folder names (0x01BB2 Metroid  Samus Returns).
    const QString title_folder =
        QStringLiteral("%1").arg(static_cast<u32>(program_id), 8, 16, QLatin1Char('0'));
    const QString unique_id =
        QStringLiteral("0x%1").arg(static_cast<u32>(program_id) >> 8, 5, 16, QLatin1Char('0'));

    std::vector<Found> found;
    const auto add = [&](Found save) {
        if (!SamePath(save.folder, own)) {
            save.this_game = save.folder.contains(title_folder, Qt::CaseInsensitive) ||
                             save.folder.contains(unique_id, Qt::CaseInsensitive);
            found.push_back(std::move(save));
        }
    };
    std::vector<std::pair<QString, int>> pending{{QDir::cleanPath(path), 0}};
    int visited = 0;
    while (!pending.empty() && visited++ < MaxFolders) {
        const auto [folder, depth] = pending.back();
        pending.pop_back();
        const auto children = Subfolders(folder);
        if (Found save = SaveAt(folder, children); save.files > 0) {
            add(std::move(save));
            continue; // a save holds no other saves
        }
        if (depth < MaxDepth) {
            for (const auto& child : children) {
                pending.emplace_back(child.absoluteFilePath(), depth + 1);
            }
        }
    }
    // A folder inside a save (profile0, highlighted when the folder dialog closed): the save
    // above it.
    if (found.empty()) {
        QDir folder(QDir::cleanPath(path));
        for (int level = 0; level < 3 && folder.cdUp(); ++level) {
            if (Found save = SaveAt(folder.path(), Subfolders(folder.path())); save.files > 0) {
                add(std::move(save));
                break;
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        return a.this_game != b.this_game ? a.this_game : a.modified > b.modified;
    });
    return found;
}

QString Import(const Found& found, u64 program_id, QString* backup) {
    backup->clear();
    const QString target = Folder(program_id);
    const QString metadata = target + QStringLiteral(".metadata");
    const QString staging = target + QStringLiteral(".import");
    const QString previous = target + QStringLiteral(".previous");
    QDir(staging).removeRecursively();
    QDir(previous).removeRecursively();

    // Copied beside the saves first, so a failed copy changes nothing.
    if (const QString error = CopyTree(found.folder, staging); !error.isEmpty()) {
        QDir(staging).removeRecursively();
        return QStringLiteral("Could not copy the saves (%1).").arg(error);
    }
    if (QDir(target).exists()) {
        const QString folder =
            GameFolder::User() + QStringLiteral("msr/save-backups/") +
            QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
        QString error = CopyTree(target, folder + QStringLiteral("/00000001"));
        if (error.isEmpty() && QFile::exists(metadata)) {
            QFile file(metadata);
            if (!file.copy(folder + QStringLiteral("/00000001.metadata"))) {
                error = file.errorString();
            }
        }
        if (!error.isEmpty()) {
            QDir(staging).removeRecursively();
            return QStringLiteral("Could not back up the current saves (%1).").arg(error);
        }
        *backup = QDir::toNativeSeparators(folder);
        if (!QDir().rename(target, previous)) {
            QDir(staging).removeRecursively();
            return QStringLiteral("Could not replace the current saves in %1. Is a window open on "
                                  "that folder, or is the game running?")
                .arg(QDir::toNativeSeparators(target));
        }
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QDir().rename(staging, target)) {
        QDir().rename(previous, target);
        QDir(staging).removeRecursively();
        return QStringLiteral("Could not move the imported saves into place.");
    }
    QDir(previous).removeRecursively();

    // The archive's format: the imported one if it came with one, else the one here, else the
    // game's own.
    if (!found.metadata.isEmpty()) {
        QFile::remove(metadata);
        QFile::copy(found.metadata, metadata);
    } else if (!QFile::exists(metadata)) {
        QFile file(metadata);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(reinterpret_cast<const char*>(DefaultMetadata.data()),
                       static_cast<qint64>(DefaultMetadata.size()));
        }
    }
    return {};
}

void OpenFolder(u64 program_id) {
    QString folder = Folder(program_id);
    while (!QDir(folder).exists() && folder.contains(QLatin1Char('/'))) {
        folder = QFileInfo(folder).absolutePath();
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

} // namespace MSR::Saves
