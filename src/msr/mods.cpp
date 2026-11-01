// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSettings>
#include <QUrl>
#include "common/logging/log.h"
#include "common/settings.h"
#include "msr/cheats.h"
#include "msr/game_folder.h"
#include "msr/game_mods.h"
#include "msr/mods.h"
#include "msr/texture_packs.h"

namespace MSR::Mods {
namespace {

/// The order and the disabled mods or packs, by folder name, in user/msr/settings.ini.
struct State {
    QStringList order;
    QStringList disabled;
};

constexpr char ModsGroup[] = "game_mods";
constexpr char PacksGroup[] = "texture_packs";

QString SettingsPath() {
    return GameFolder::User() + QStringLiteral("msr/settings.ini");
}

State Read(const char* group) {
    QSettings file(SettingsPath(), QSettings::IniFormat);
    file.beginGroup(QLatin1String(group));
    return {file.value(QStringLiteral("order")).toStringList(),
            file.value(QStringLiteral("disabled")).toStringList()};
}

template <typename Item>
void Write(const char* group, const std::vector<Item>& items) {
    QStringList order, disabled;
    for (const auto& item : items) {
        order << item.name;
        if (!item.enabled) {
            disabled << item.name;
        }
    }
    QSettings file(SettingsPath(), QSettings::IniFormat);
    file.beginGroup(QLatin1String(group));
    file.setValue(QStringLiteral("order"), order);
    file.setValue(QStringLiteral("disabled"), disabled);
}

template <typename Item>
void SetEnabledIn(const char* group, std::vector<Item> items, const QString& name, bool enabled) {
    for (auto& item : items) {
        if (item.name == name) {
            item.enabled = enabled;
        }
    }
    Write(group, items);
}

template <typename Item>
void MoveIn(const char* group, std::vector<Item> items, const QString& name, int direction) {
    const auto it = std::find_if(items.begin(), items.end(),
                                 [&](const Item& item) { return item.name == name; });
    if (it == items.end()) {
        return;
    }
    const auto index = it - items.begin();
    const auto other = index + direction;
    if (other < 0 || other >= static_cast<decltype(other)>(items.size())) {
        return;
    }
    std::swap(items[index], items[other]);
    Write(group, items);
}

QString Count(int count, const char* one, const char* many) {
    return count == 1 ? QStringLiteral("1 %1").arg(QLatin1String(one))
                      : QStringLiteral("%1 %2").arg(QLocale().toString(count), QLatin1String(many));
}

int CountFiles(const QString& path, const QStringList& filters = {}) {
    int files = 0;
    QDirIterator it(path, filters, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        ++files;
    }
    return files;
}

/// What an emulator's load/mods/<title> folder holds.
const QStringList ModEntries = {
    QStringLiteral("romfs"),        QStringLiteral("romfs_ext"),  QStringLiteral("exefs"),
    QStringLiteral("code.bin"),     QStringLiteral("code.ips"),   QStringLiteral("code.bps"),
    QStringLiteral("exheader.bin"), QStringLiteral("banner.bnr"), QStringLiteral("icon.bin")};

/// The folder holding a mod's files: the mod's own, else the first one inside it, up to three
/// levels down, as in a mod packed as 00040000001BB200/romfs or luma/titles/00040000001BB200/.
QString FindModFiles(const QString& path, int depth = 3) {
    const QDir folder(path);
    if (std::any_of(ModEntries.begin(), ModEntries.end(),
                    [&](const QString& entry) { return folder.exists(entry); })) {
        return path;
    }
    if (depth == 0) {
        return {};
    }
    const auto flags = QDir::Name | QDir::IgnoreCase;
    for (const QString& name : folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot, flags)) {
        if (QString found = FindModFiles(path + name + QLatin1Char('/'), depth - 1);
            !found.isEmpty()) {
            return found;
        }
    }
    return {};
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
    return CountFiles(path, TextureFiles);
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

QString GameModFolder() {
    const QString path = GameFolder::User() + QStringLiteral("mods/game/");
    QDir().mkpath(path);
    return path;
}

namespace {

/// Randovania exports a randomizer for Azahar to Azahar's own mods folder by default (Windows):
/// listed as one more mod, under this name, off until the player turns it on, as it may have
/// been meant for Azahar.
const QString AzaharModName = QStringLiteral("Azahar's mods folder");

QString AzaharModFolder() {
#ifdef _WIN32
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty()) {
        const QString path = QDir::fromNativeSeparators(appdata) +
                             QStringLiteral("/Azahar/load/mods/00040000001BB200/");
        if (!FindModFiles(path).isEmpty()) {
            return path;
        }
    }
#endif
    return {};
}

} // namespace

std::vector<GameMod> GameMods() {
    const QString root = GameModFolder();
    QStringList names =
        QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase);
    const QString azahar = AzaharModFolder();
    if (!azahar.isEmpty() && !names.contains(AzaharModName)) {
        names << AzaharModName;
    }
    const State state = Read(ModsGroup);
    std::vector<GameMod> mods;
    const auto add = [&](const QString& name) {
        if (name == AzaharModName && !azahar.isEmpty()) {
            mods.push_back({name, azahar,
                            state.order.contains(name) && !state.disabled.contains(name)});
            return;
        }
        mods.push_back({name, FindModFiles(root + name + QLatin1Char('/')),
                        !state.disabled.contains(name)});
    };
    // New mods go first, as the one just added is meant to show.
    for (const QString& name : names) {
        if (!state.order.contains(name)) {
            add(name);
        }
    }
    for (const QString& name : state.order) {
        if (names.contains(name)) {
            add(name);
        }
    }
    return mods;
}

QString Describe(const GameMod& mod) {
    if (mod.path.isEmpty()) {
        return QStringLiteral("no mod files found: a mod has romfs, exefs or a code patch");
    }
    const QDir folder(mod.path);
    const auto has = [&](const char* file) {
        return folder.exists(QLatin1String(file)) ||
               folder.exists(QStringLiteral("exefs/") + QLatin1String(file));
    };
    QStringList parts;
    if (const int files = CountFiles(mod.path + QStringLiteral("romfs/")); files > 0) {
        parts << Count(files, "file", "files");
    }
    const QString ext = mod.path + QStringLiteral("romfs_ext/");
    if (const int patches = CountFiles(ext, {QStringLiteral("*.ips"), QStringLiteral("*.bps")});
        patches > 0) {
        parts << Count(patches, "file patch", "file patches");
    }
    if (const int stubs = CountFiles(ext, {QStringLiteral("*.stub")}); stubs > 0) {
        parts << Count(stubs, "file removed", "files removed");
    }
    if (has("code.bin")) {
        parts << QStringLiteral("new game code");
    }
    if (has("code.ips") || has("code.bps")) {
        parts << QStringLiteral("code patch");
    }
    if (folder.exists(QStringLiteral("exheader.bin"))) {
        parts << QStringLiteral("exheader");
    }
    if (parts.isEmpty()) {
        return QStringLiteral("changes nothing in the game");
    }
    // A Randovania export keeps its patcher settings beside the files (except for races): the
    // seed's name, as the new file screen shows it.
    QFile patcher(mod.path + QStringLiteral("patcher.json"));
    if (patcher.open(QIODevice::ReadOnly)) {
        const QJsonObject settings = QJsonDocument::fromJson(patcher.readAll()).object();
        const QString seed = settings.value(QStringLiteral("text_patches"))
                                 .toObject()
                                 .value(QStringLiteral("GUI_MSG_NEW_FILE_CREATION"))
                                 .toString();
        return (seed.isEmpty() ? QStringLiteral("Randovania seed")
                               : QStringLiteral("Randovania seed %1").arg(seed)) +
               QStringLiteral(", for a new file: ") + parts.join(QStringLiteral(", "));
    }
    return parts.join(QStringLiteral(", "));
}

bool IsRandomizer(const GameMod& mod) {
    return !mod.path.isEmpty() &&
           QFileInfo(mod.path + QStringLiteral("romfs/actors/items/randomizerpowerup")).isDir();
}

bool RandomizerEnabled() {
    const auto mods = GameMods();
    return std::any_of(mods.begin(), mods.end(),
                       [](const GameMod& mod) { return mod.enabled && IsRandomizer(mod); });
}

void SetModEnabled(const QString& name, bool enabled) {
    SetEnabledIn(ModsGroup, GameMods(), name, enabled);
}

void MoveMod(const QString& name, int direction) {
    MoveIn(ModsGroup, GameMods(), name, direction);
}

void OpenModFolder() {
    QDesktopServices::openUrl(QUrl::fromLocalFile(GameModFolder()));
}

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
    const State state = Read(PacksGroup);
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
    SetEnabledIn(PacksGroup, TexturePacks(), name, enabled);
}

void Move(const QString& name, int direction) {
    MoveIn(PacksGroup, TexturePacks(), name, direction);
}

QString Pending() {
    QStringList lines;
    for (const auto& mod : GameMods()) {
        if (mod.enabled && !mod.path.isEmpty()) {
            lines << QStringLiteral("mod:") + mod.path;
        }
    }
    for (const auto& pack : TexturePacks()) {
        if (pack.enabled) {
            lines << pack.name;
        }
    }
    lines << QStringLiteral("preload=%1").arg(Settings::values.preload_textures.GetValue());
    lines << QStringLiteral("dump=%1").arg(Settings::values.dump_textures.GetValue());
    return lines.join(QLatin1Char('\n'));
}

QString Publish(u64 program_id) {
    std::vector<std::string> mods;
    bool randomizer = false;
    for (const auto& mod : GameMods()) {
        if (mod.enabled && !mod.path.isEmpty()) {
            LOG_INFO(Frontend, "Game mod: {}", mod.path.toStdString());
            mods.push_back(mod.path.toStdString());
            randomizer |= IsRandomizer(mod);
        }
    }
    MSR::GameMods::Set(program_id, std::move(mods));
    MSR::Cheats::randomizer = randomizer;

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
