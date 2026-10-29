// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSettings>
#include <QUrl>
#include "common/settings.h"
#include "msr/game_folder.h"
#include "msr/mods.h"
#include "msr/texture_packs.h"

namespace MSR::Mods {
namespace {

/// The order and the disabled packs, by folder name, in user/msr/settings.ini.
struct State {
    QStringList order;
    QStringList disabled;
};

QString SettingsPath() {
    return GameFolder::User() + QStringLiteral("msr/settings.ini");
}

State Read() {
    QSettings file(SettingsPath(), QSettings::IniFormat);
    file.beginGroup(QStringLiteral("texture_packs"));
    return {file.value(QStringLiteral("order")).toStringList(),
            file.value(QStringLiteral("disabled")).toStringList()};
}

void Write(const std::vector<TexturePack>& packs) {
    QStringList order, disabled;
    for (const auto& pack : packs) {
        order << pack.name;
        if (!pack.enabled) {
            disabled << pack.name;
        }
    }
    QSettings file(SettingsPath(), QSettings::IniFormat);
    file.beginGroup(QStringLiteral("texture_packs"));
    file.setValue(QStringLiteral("order"), order);
    file.setValue(QStringLiteral("disabled"), disabled);
}

/// The pack.json nearest the pack's top folder, if any.
QString FindConfig(const QString& path) {
    if (QFile::exists(path + QStringLiteral("pack.json"))) {
        return path + QStringLiteral("pack.json");
    }
    QDirIterator it(path, {QStringLiteral("pack.json")}, QDir::Files, QDirIterator::Subdirectories);
    QString best;
    while (it.hasNext()) {
        const QString found = it.next();
        if (best.isEmpty() || found.count(QLatin1Char('/')) < best.count(QLatin1Char('/'))) {
            best = found;
        }
    }
    return best;
}

const QStringList TextureFiles = {QStringLiteral("*.png"), QStringLiteral("*.dds"),
                                  QStringLiteral("*.ktx")};

int CountTextures(const QString& path) {
    int textures = 0;
    QDirIterator it(path, TextureFiles, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++textures;
    }
    return textures;
}

/// The pack folders, relative to the packs folder. A folder holding only folders (no textures
/// or pack.json of its own) is a set of packs, like a pack's "Optional Addons": each one inside
/// is listed on its own. A pack sorted into category folders shows as several packs that way,
/// which loads the same.
QStringList FindPacks(const QString& root) {
    QStringList packs;
    const auto flags = QDir::Name | QDir::IgnoreCase;
    for (const QString& name : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, flags)) {
        const QDir folder(root + name);
        const QStringList inner = folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot, flags);
        const bool container = !inner.isEmpty() && !folder.exists(QStringLiteral("pack.json")) &&
                               folder.entryList(TextureFiles, QDir::Files).isEmpty();
        if (!container) {
            packs << name;
            continue;
        }
        for (const QString& sub : inner) {
            packs << name + QLatin1Char('/') + sub;
        }
    }
    return packs;
}

} // namespace

QString TexturePackFolder() {
    const QString path = GameFolder::User() + QStringLiteral("mods/textures/");
    QDir().mkpath(path);
    return path;
}

std::vector<TexturePack> TexturePacks() {
    // The player's packs, then those shipping with the game that no player's pack replaces.
    const QString root = TexturePackFolder();
    QStringList names = FindPacks(root);
    QStringList built_in;
    if (const QString game = GameFolder::BuiltInTextures(); !game.isEmpty()) {
        for (const QString& name : FindPacks(game)) {
            if (!names.contains(name)) {
                built_in << name;
            }
        }
    }
    names << built_in;
    const auto folder = [&](const QString& name) {
        return (built_in.contains(name) ? GameFolder::BuiltInTextures() : root) + name +
               QLatin1Char('/');
    };
    const State state = Read();
    std::vector<TexturePack> packs;
    const auto add = [&](const QString& name) {
        packs.push_back({name, folder(name), !state.disabled.contains(name),
                         built_in.contains(name)});
    };
    // New packs go first, as the one just added is meant to show; smaller ones before larger
    // ones, so add-ons override the packs they extend.
    std::vector<std::pair<int, QString>> added;
    for (const QString& name : names) {
        if (!state.order.contains(name)) {
            added.emplace_back(CountTextures(folder(name)), name);
        }
    }
    std::stable_sort(added.begin(), added.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [count, name] : added) {
        add(name);
    }
    for (const QString& name : state.order) {
        if (names.contains(name)) {
            add(name);
        }
    }
    return packs;
}

QString Describe(const TexturePack& pack) {
    const int textures = CountTextures(pack.path);
    QString text = textures == 0 ? QStringLiteral("no textures found")
                   : textures == 1
                       ? QStringLiteral("1 texture")
                       : QStringLiteral("%1 textures").arg(QLocale().toString(textures));
    if (pack.built_in) {
        text += QStringLiteral(", included with the game");
    }
    if (const QString config = FindConfig(pack.path); !config.isEmpty()) {
        QFile file(config);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
            const QString author = json.value(QStringLiteral("author")).toString();
            const QString version = json.value(QStringLiteral("version")).toString();
            // Azahar's dump template says "citra" and 1.0.0: not worth showing.
            if (!author.isEmpty() && author != QStringLiteral("citra")) {
                text += QStringLiteral(", by %1").arg(author);
            }
            if (!version.isEmpty() && version != QStringLiteral("1.0.0")) {
                text += QStringLiteral(", v%1").arg(version);
            }
        }
    }
    return text;
}

void SetEnabled(const QString& name, bool enabled) {
    auto packs = TexturePacks();
    for (auto& pack : packs) {
        if (pack.name == name) {
            pack.enabled = enabled;
        }
    }
    Write(packs);
}

void Move(const QString& name, int direction) {
    auto packs = TexturePacks();
    const auto it = std::find_if(packs.begin(), packs.end(),
                                 [&](const TexturePack& pack) { return pack.name == name; });
    if (it == packs.end()) {
        return;
    }
    const auto index = it - packs.begin();
    const auto other = index + direction;
    if (other < 0 || other >= static_cast<decltype(other)>(packs.size())) {
        return;
    }
    std::swap(packs[index], packs[other]);
    Write(packs);
}

QString Pending() {
    QStringList lines;
    for (const auto& pack : TexturePacks()) {
        if (pack.enabled) {
            lines << pack.name;
        }
    }
    lines << QStringLiteral("preload=%1").arg(Settings::values.preload_textures.GetValue());
    lines << QStringLiteral("dump=%1").arg(Settings::values.dump_textures.GetValue());
    return lines.join(QLatin1Char('\n'));
}

QString Publish() {
    std::vector<std::string> folders;
    for (const auto& pack : TexturePacks()) {
        if (pack.enabled) {
            folders.push_back(pack.path.toStdString());
        }
    }
    Settings::values.custom_textures = !folders.empty();
    MSR::TexturePacks::SetFolders(std::move(folders));
    return Pending();
}

void OpenFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(TexturePackFolder()));
}

} // namespace MSR::Mods
