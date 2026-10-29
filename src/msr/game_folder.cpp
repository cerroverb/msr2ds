// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtEndian>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "msr/game_folder.h"

namespace MSR::GameFolder {
namespace {

constexpr qint64 MediaUnit = 0x200;
bool first_run = false;
QString active_rom;

QString RememberedRomFile() {
    return User() + QStringLiteral("msr/game-path.txt");
}

bool IsDumpSuffix(const QFileInfo& file) {
    const auto suffix = file.suffix().toLower();
    return suffix == QStringLiteral("3ds") || suffix == QStringLiteral("cci") ||
           suffix == QStringLiteral("cxi");
}

QString FindIn(const QString& folder) {
    for (const auto& file : QDir(folder).entryInfoList(QDir::Files, QDir::Name)) {
        if (IsDumpSuffix(file) && Probe(file.absoluteFilePath()).Ok()) {
            return file.absoluteFilePath();
        }
    }
    return {};
}

} // namespace

QString RomInfo::Region() const {
    if (product_code.size() < 10) {
        return QStringLiteral("unknown region");
    }
    switch (product_code.at(9).toLatin1()) {
    case 'E':
        return QStringLiteral("USA");
    case 'P':
        return QStringLiteral("Europe");
    case 'J':
        return QStringLiteral("Japan");
    default:
        return product_code.mid(9, 1);
    }
}

QString Root() {
    return QCoreApplication::applicationDirPath();
}

QString User() {
    static const QString user = [] {
        // An AppImage runs from a read-only mount: its data goes to a user/ folder beside the
        // AppImage file when there is one (portable), otherwise to the user's data folder.
        const QString appimage = qEnvironmentVariable("APPIMAGE");
        if (appimage.isEmpty()) {
            return Root() + QStringLiteral("/user/");
        }
        const QString portable = QFileInfo(appimage).absolutePath() + QStringLiteral("/user/");
        if (QFileInfo(portable).isDir()) {
            return portable;
        }
        return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
               QStringLiteral("/samus-returns-recomp/");
    }();
    return user;
}

QString BuiltInTextures() {
    for (const QString& folder : {Root() + QStringLiteral("/textures/"),
                                  Root() + QStringLiteral("/../share/samus-returns-recomp/textures/")}) {
        if (QFileInfo(folder).isDir()) {
            return QDir::cleanPath(folder) + QLatin1Char('/');
        }
    }
    return {};
}

void Prepare() {
    QDir().mkpath(User());
    first_run = !QFileInfo::exists(User() + QStringLiteral("config/qt-config.ini"));
    FileUtil::SetUserPath(User().toStdString());
}

bool IsFirstRun() {
    return first_run;
}

RomInfo Probe(const QString& path) {
    RomInfo info;
    info.path = path;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        info.error = QStringLiteral("Could not open the file.");
        return info;
    }
    if (path.endsWith(QStringLiteral(".cia"), Qt::CaseInsensitive)) {
        info.error = QStringLiteral("CIA installers are not supported. Use a game-card dump "
                                    "(.3ds or .cci).");
        return info;
    }

    const QByteArray header = file.read(MediaUnit);
    if (header.size() != MediaUnit) {
        info.error = QStringLiteral("The file is too small to be a 3DS game.");
        return info;
    }

    qint64 ncch_offset = 0;
    if (header.mid(0x100, 4) == "NCSD") {
        // Partition 0 holds the game; the others are the manual, download play and updates.
        ncch_offset = qFromLittleEndian<quint32>(header.constData() + 0x120) * MediaUnit;
        const qint64 ncch_size = qFromLittleEndian<quint32>(header.constData() + 0x124) * MediaUnit;
        if (ncch_offset < MediaUnit || ncch_offset + ncch_size > file.size()) {
            info.error = QStringLiteral("The game dump is incomplete or truncated.");
            return info;
        }
    } else if (header.mid(0x100, 4) != "NCCH") {
        info.error = QStringLiteral("Not a 3DS game dump (.3ds, .cci or .cxi).");
        return info;
    }

    QByteArray ncch = header;
    if (ncch_offset != 0) {
        file.seek(ncch_offset);
        ncch = file.read(MediaUnit);
    }
    if (ncch.size() != MediaUnit || ncch.mid(0x100, 4) != "NCCH") {
        info.error = QStringLiteral("The game partition is missing or damaged.");
        return info;
    }

    info.program_id = qFromLittleEndian<quint64>(ncch.constData() + 0x118);
    info.product_code = QString::fromLatin1(ncch.mid(0x150, 16)).remove(QChar(u'\0')).trimmed();
    // NCCH flags[7] bit 2 is NoCrypto. The loader only accepts decrypted games.
    info.encrypted = (static_cast<u8>(ncch.at(0x188 + 7)) & 0x4) == 0;

    if (!info.product_code.startsWith(QString::fromLatin1(ProductCodePrefix))) {
        info.error = QStringLiteral("This is not Metroid: Samus Returns (found %1, title %2).")
                         .arg(info.product_code.isEmpty() ? QStringLiteral("no product code")
                                                          : info.product_code)
                         .arg(info.program_id, 16, 16, QChar(u'0'));
    } else if (info.encrypted) {
        info.error = QStringLiteral("This dump is encrypted. Only decrypted dumps can be played; "
                                    "decrypt it first (for example with GodMode9).");
    }
    return info;
}

QString FindRom() {
    if (auto rom = FindIn(Root()); !rom.isEmpty()) {
        return rom;
    }
    if (auto rom = FindIn(Root() + QStringLiteral("/game")); !rom.isEmpty()) {
        return rom;
    }
    QFile remembered(RememberedRomFile());
    if (remembered.open(QIODevice::ReadOnly)) {
        const auto path = QString::fromUtf8(remembered.readAll()).trimmed();
        if (!path.isEmpty() && Probe(path).Ok()) {
            return path;
        }
        LOG_WARNING(Frontend, "Remembered game {} is no longer usable", path.toStdString());
    }
    return {};
}

void RememberRom(const QString& path) {
    QDir().mkpath(QFileInfo(RememberedRomFile()).absolutePath());
    QFile remembered(RememberedRomFile());
    if (remembered.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        remembered.write(QDir::toNativeSeparators(path).toUtf8());
    }
}

void SetActiveRom(const QString& path) {
    active_rom = path;
}

QString ActiveRom() {
    return active_rom;
}

} // namespace MSR::GameFolder
