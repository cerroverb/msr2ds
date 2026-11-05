// Game mods and texture packs on msr-native (mods.h): src/msr/mods.cpp without Qt, reading and writing the same
// settings.ini groups, so the launcher, the emulator-based build and msr-native list them alike.
#include "mods.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include "ini.h"
#include "msr.h"
#include "patch.h"
#include "msr/cheats.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace NativeMods {

fs::path P(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

namespace {

std::string user, settings, exe;
std::vector<std::string> booted_mods;
std::string booted;

std::string S(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsDir(const fs::path& p) {
    std::error_code e;
    return fs::is_directory(p, e);
}

bool Exists(const fs::path& p) {
    std::error_code e;
    return fs::exists(p, e);
}

/// Subfolders by name, ignoring case (QDir::Name | QDir::IgnoreCase).
std::vector<std::string> Folders(const fs::path& root) {
    std::vector<std::string> names;
    std::error_code e;
    for (fs::directory_iterator it(root, e), end; !e && it != end; it.increment(e)) {
        std::error_code e2;
        if (it->is_directory(e2)) names.push_back(S(it->path().filename()));
    }
    std::stable_sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    return names;
}

bool Contains(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

struct State {
    std::vector<std::string> order, disabled;
};

State Read(const char* group) {
    const MsrIni::File file(settings);
    return {file.List(group, "order"), file.List(group, "disabled")};
}

template <typename Item>
void Write(const char* group, const std::vector<Item>& items) {
    std::vector<std::string> order, disabled;
    for (const auto& item : items) {
        order.push_back(item.name);
        if (!item.enabled) disabled.push_back(item.name);
    }
    MsrIni::File file(settings);
    file.SetList(group, "order", order);
    file.SetList(group, "disabled", disabled);
    if (!file.Save()) std::fprintf(stderr, "msr: could not write %s\n", settings.c_str());
}

template <typename Item>
void SetEnabledIn(const char* group, std::vector<Item> items, const std::string& name, bool enabled) {
    for (auto& item : items) {
        if (item.name == name) item.enabled = enabled;
    }
    Write(group, items);
}

template <typename Item>
void MoveIn(const char* group, std::vector<Item> items, const std::string& name, int direction) {
    const auto it = std::find_if(items.begin(), items.end(), [&](const Item& item) { return item.name == name; });
    if (it == items.end()) return;
    const auto index = it - items.begin(), other = index + direction;
    if (other < 0 || other >= static_cast<decltype(other)>(items.size())) return;
    std::swap(items[static_cast<std::size_t>(index)], items[static_cast<std::size_t>(other)]);
    Write(group, items);
}

std::string Count(int count, const char* one, const char* many) {
    if (count == 1) return std::string("1 ") + one;
    std::string digits = std::to_string(count), grouped;   // as QLocale writes it: 1,234
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) grouped += ',';
        grouped += digits[i];
    }
    return grouped + " " + many;
}

/// Files below `root`, those with one of `extensions` (lower case, with the dot) when given.
int CountFiles(const fs::path& root, const std::vector<std::string>& extensions = {}) {
    int files = 0;
    std::error_code e;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, e), end; !e && it != end;
         it.increment(e)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        if (extensions.empty() || Contains(extensions, Lower(S(it->path().extension())))) ++files;
    }
    return files;
}

const char* const ModEntries[] = {"romfs", "romfs_ext", "exefs", "code.bin", "code.ips", "code.bps",
                                  "exheader.bin", "banner.bnr", "icon.bin"};

/// The folder holding a mod's files: its own, else the first one inside it, up to three levels down.
std::string FindModFiles(const std::string& path, int depth = 3) {
    const fs::path folder = P(path);
    for (const char* entry : ModEntries) {
        if (Exists(folder / entry)) return path;
    }
    if (depth == 0) return {};
    for (const std::string& name : Folders(folder)) {
        if (std::string found = FindModFiles(path + name + "/", depth - 1); !found.empty()) return found;
    }
    return {};
}

const std::vector<std::string> TextureExtensions = {".png", ".dds", ".ktx"};

int CountTextures(const std::string& path) {
    return CountFiles(P(path), TextureExtensions);
}

/// The pack folders, relative to the packs folder; a folder holding only folders is a set of packs.
std::vector<std::string> FindPacks(const std::string& root) {
    std::vector<std::string> packs;
    for (const std::string& name : Folders(P(root))) {
        const fs::path folder = P(root + name);
        const auto inner = Folders(folder);
        bool textures = false;
        std::error_code e;
        for (fs::directory_iterator it(folder, e), end; !e && it != end && !textures; it.increment(e)) {
            std::error_code e2;
            textures = it->is_regular_file(e2) && Contains(TextureExtensions, Lower(S(it->path().extension())));
        }
        const bool container = !inner.empty() && !Exists(folder / "pack.json") && !textures;
        if (!container) {
            packs.push_back(name);
            continue;
        }
        for (const std::string& sub : inner) packs.push_back(name + "/" + sub);
    }
    return packs;
}

std::string ReadText(const fs::path& path) {
    std::string text;
#ifdef _WIN32
    std::FILE* f = _wfopen(path.c_str(), L"rb");
#else
    std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return text;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (8u << 20)) text.append(buf, n);
    std::fclose(f);
    return text;
}

/// A JSON string value after "key": (enough for patcher.json and pack.json).
std::string JsonString(const std::string& json, const std::string& key) {
    const auto at = json.find("\"" + key + "\"");
    if (at == std::string::npos) return {};
    std::size_t i = json.find(':', at + key.size() + 2);
    if (i == std::string::npos) return {};
    while (++i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {}
    if (i >= json.size() || json[i] != '"') return {};
    std::string out;
    for (++i; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] != '\\' || i + 1 >= json.size()) {
            out += json[i];
            continue;
        }
        const char e = json[++i];
        if (e == 'u' && i + 4 < json.size()) {
            const unsigned code = static_cast<unsigned>(std::stoul(json.substr(i + 1, 4), nullptr, 16));
            i += 4;
            if (code < 0x80) out += static_cast<char>(code);
            else if (code < 0x800) { out += static_cast<char>(0xC0 | code >> 6); out += static_cast<char>(0x80 | (code & 0x3F)); }
            else { out += static_cast<char>(0xE0 | code >> 12); out += static_cast<char>(0x80 | (code >> 6 & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        } else {
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
        }
    }
    return out;
}

const std::string AzaharModName = "Azahar's mods folder";

std::string AzaharModFolder() {
#ifdef _WIN32
    if (const wchar_t* appdata = _wgetenv(L"APPDATA")) {
        const std::string path = S(fs::path(appdata) / "Azahar/load/mods/00040000001BB200") + "/";
        if (!FindModFiles(path).empty()) return path;
    }
#endif
    return {};
}

std::string BuiltInTextures() {
    for (const std::string& folder : {exe + "textures/", exe + "../share/samus-returns-recomp/textures/"}) {
        if (IsDir(P(folder))) return S(fs::weakly_canonical(P(folder))) + "/";
    }
    return {};
}

} // namespace

void Init(const std::string& user_folder, const std::string& settings_file, const std::string& exe_folder) {
    std::error_code e;
    user = S(fs::absolute(P(user_folder), e).lexically_normal());
    if (!user.empty() && user.back() != '/') user += '/';
    settings = settings_file;
    exe = exe_folder;
}

std::string GameModFolder() {
    const std::string path = user + "mods/game/";
    std::error_code e;
    fs::create_directories(P(path), e);
    return path;
}

std::vector<GameMod> GameMods() {
    const std::string root = GameModFolder();
    std::vector<std::string> names = Folders(P(root));
    const std::string azahar = AzaharModFolder();
    if (!azahar.empty() && !Contains(names, AzaharModName)) names.push_back(AzaharModName);
    const State state = Read("game_mods");
    std::vector<GameMod> mods;
    const auto add = [&](const std::string& name) {
        if (name == AzaharModName && !azahar.empty()) {
            mods.push_back({name, azahar, Contains(state.order, name) && !Contains(state.disabled, name)});
            return;
        }
        mods.push_back({name, FindModFiles(root + name + "/"), !Contains(state.disabled, name)});
    };
    for (const std::string& name : names) {   // new mods first, as the one just added is meant to show
        if (!Contains(state.order, name)) add(name);
    }
    for (const std::string& name : state.order) {
        if (Contains(names, name)) add(name);
    }
    return mods;
}

std::string Describe(const GameMod& mod) {
    if (mod.path.empty()) return "no mod files found: a mod has romfs, exefs or a code patch";
    const fs::path folder = P(mod.path);
    const auto has = [&](const char* file) { return Exists(folder / file) || Exists(folder / "exefs" / file); };
    std::vector<std::string> parts;
    if (const int files = CountFiles(folder / "romfs"); files > 0) parts.push_back(Count(files, "file", "files"));
    if (const int patches = CountFiles(folder / "romfs_ext", {".ips", ".bps"}); patches > 0)
        parts.push_back(Count(patches, "file patch", "file patches"));
    if (const int stubs = CountFiles(folder / "romfs_ext", {".stub"}); stubs > 0)
        parts.push_back(Count(stubs, "file removed", "files removed"));
    if (has("code.bin")) parts.push_back("new game code");
    if (has("code.ips") || has("code.bps")) parts.push_back("code patch");
    if (Exists(folder / "exheader.bin")) parts.push_back("exheader");
    if (parts.empty()) return "changes nothing in the game";
    std::string joined;
    for (const std::string& part : parts) joined += (joined.empty() ? "" : ", ") + part;
    if (const std::string patcher = ReadText(folder / "patcher.json"); !patcher.empty()) {
        const std::string seed = JsonString(patcher, "GUI_MSG_NEW_FILE_CREATION");
        return (seed.empty() ? std::string("Randovania seed") : "Randovania seed " + seed) + ", for a new file: " + joined;
    }
    return joined;
}

bool IsRandomizer(const GameMod& mod) {
    return !mod.path.empty() && IsDir(P(mod.path) / "romfs/actors/items/randomizerpowerup");
}

void SetModEnabled(const std::string& name, bool enabled) {
    SetEnabledIn("game_mods", GameMods(), name, enabled);
}

void MoveMod(const std::string& name, int direction) {
    MoveIn("game_mods", GameMods(), name, direction);
}

std::string TexturePackFolder() {
    const std::string path = user + "mods/textures/";
    std::error_code e;
    fs::create_directories(P(path), e);
    return path;
}

std::vector<TexturePack> TexturePacks() {
    const std::string root = TexturePackFolder(), game = BuiltInTextures();
    std::vector<std::string> names = FindPacks(root), built_in;
    if (!game.empty()) {
        for (const std::string& name : FindPacks(game)) {
            if (!Contains(names, name)) built_in.push_back(name);
        }
    }
    names.insert(names.end(), built_in.begin(), built_in.end());
    const auto folder = [&](const std::string& name) { return (Contains(built_in, name) ? game : root) + name + "/"; };
    const State state = Read("texture_packs");
    std::vector<TexturePack> packs;
    const auto add = [&](const std::string& name) {
        packs.push_back({name, folder(name), !Contains(state.disabled, name), Contains(built_in, name)});
    };
    // New packs first, smaller ones before larger ones, so add-ons override the packs they extend.
    std::vector<std::pair<int, std::string>> added;
    for (const std::string& name : names) {
        if (!Contains(state.order, name)) added.emplace_back(CountTextures(folder(name)), name);
    }
    std::stable_sort(added.begin(), added.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& item : added) add(item.second);
    for (const std::string& name : state.order) {
        if (Contains(names, name)) add(name);
    }
    return packs;
}

std::string Describe(const TexturePack& pack) {
    const int textures = CountTextures(pack.path);
    std::string text = textures == 0 ? "no textures found" : Count(textures, "texture", "textures");
    if (pack.built_in) text += ", included with the game";
    fs::path config = P(pack.path) / "pack.json";
    if (!Exists(config)) {   // the nearest one below
        config.clear();
        std::size_t depth = 1000;
        std::error_code e;
        for (fs::recursive_directory_iterator it(P(pack.path), fs::directory_options::skip_permission_denied, e), end;
             !e && it != end; it.increment(e)) {
            if (it->path().filename() == "pack.json" && static_cast<std::size_t>(it.depth()) < depth) {
                depth = static_cast<std::size_t>(it.depth());
                config = it->path();
            }
        }
    }
    if (!config.empty()) {
        const std::string json = ReadText(config);
        const std::string author = JsonString(json, "author"), version = JsonString(json, "version");
        if (!author.empty() && author != "citra") text += ", by " + author;
        if (!version.empty() && version != "1.0.0") text += ", v" + version;
    }
    return text;
}

void SetPackEnabled(const std::string& name, bool enabled) {
    SetEnabledIn("texture_packs", TexturePacks(), name, enabled);
}

void MovePack(const std::string& name, int direction) {
    MoveIn("texture_packs", TexturePacks(), name, direction);
}

std::string Pending() {
    std::string lines;
    for (const auto& mod : GameMods()) {
        if (mod.enabled && !mod.path.empty()) lines += "mod:" + mod.path + "\n";
    }
    for (const auto& pack : TexturePacks()) {
        if (pack.enabled) lines += pack.name + "\n";
    }
    return lines;
}

void Boot(bool packs_given) {
    bool randomizer = false;
    booted_mods.clear();
    for (const auto& mod : GameMods()) {
        if (!mod.enabled || mod.path.empty()) continue;
        std::fprintf(stderr, "msr: game mod %s\n", mod.path.c_str());
        booted_mods.push_back(mod.path);
        randomizer |= IsRandomizer(mod);
    }
    MSR::Cheats::randomizer = randomizer;
    if (!packs_given) {
        for (const auto& pack : TexturePacks()) {
            if (pack.enabled) msr_textures_add_pack(pack.path.c_str());
        }
    }
    booted = Pending();
}

const std::vector<std::string>& BootedMods() {
    return booted_mods;
}

const std::string& Booted() {
    return booted;
}

void OpenFolder(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = P(path).make_preferred().wstring();
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    pid_t pid;
    char* const args[] = {const_cast<char*>("xdg-open"), const_cast<char*>(path.c_str()), nullptr};
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, args, environ) == 0) waitpid(pid, nullptr, WNOHANG);
#endif
}

} // namespace NativeMods

// ---- the game mods' exheader and code, as the emulator loads them (src/core/file_sys/ncch_container.cpp) ----
namespace {

std::FILE* OpenFile(const std::string& path) {
#ifdef _WIN32
    return _wfopen(NativeMods::P(path).c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* f = OpenFile(path);
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<std::size_t>(size) : 0);
    const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

} // namespace

// The exheader.bin of the first enabled mod that has one: its first `size` bytes over `ex`. 1 when replaced.
extern "C" int msr_mods_exheader(uint8_t* ex, size_t size) {
    for (const std::string& mod : NativeMods::BootedMods()) {
        std::vector<std::uint8_t> data;
        if (!ReadFile(mod + "exheader.bin", data) || data.size() < 0x200) continue;
        std::memcpy(ex, data.data(), std::min(size, data.size()));
        std::fprintf(stderr, "msr: exheader from %s\n", mod.c_str());
        return 1;
    }
    return 0;
}

// The code with the game mods: the first enabled mod's code.bin (exefs/code.bin, then code.bin) instead of the dump's,
// then each mod's patch (the first of exefs/code.ips, exefs/code.bps, code.ips, code.bps it has): BPS ones first, as
// they fit only the code they were made from, then IPS ones from the lowest priority up. `*code` (malloc'd, `*size`
// bytes, `room` bytes allocated: the code and the .bss pages) is replaced when a mod changes it. 1 when changed,
// 0 when no mod touches the code, -1 when a patch does not fit.
extern "C" int msr_mods_code(uint8_t** code, uint32_t* size, uint32_t room) {
    const auto& mods = NativeMods::BootedMods();
    std::vector<std::uint8_t> buffer(*code, *code + room);
    std::size_t used = *size;
    bool changed = false;
    for (const std::string& mod : mods) {
        std::vector<std::uint8_t> replacement;
        if (ReadFile(mod + "exefs/code.bin", replacement) || ReadFile(mod + "code.bin", replacement)) {
            std::fprintf(stderr, "msr: code.bin from %s (%zu bytes)\n", mod.c_str(), replacement.size());
            const std::size_t bss = room - *size;
            used = replacement.size();
            buffer = std::move(replacement);
            buffer.resize(used + bss, 0);
            changed = true;
            break;
        }
    }
    struct Patch {
        std::string path;
        bool bps;
    };
    std::vector<Patch> patches;
    for (auto mod = mods.rbegin(); mod != mods.rend(); ++mod) {
        for (const auto& [name, bps] : {std::pair{"exefs/code.ips", false}, {"exefs/code.bps", true}, {"code.ips", false},
                                        {"code.bps", true}}) {
            std::error_code e;
            if (fs::exists(NativeMods::P(*mod + name), e)) {
                patches.push_back({*mod + name, bps});
                break;
            }
        }
    }
    std::stable_partition(patches.begin(), patches.end(), [](const Patch& p) { return p.bps; });
    for (const Patch& patch : patches) {
        std::vector<std::uint8_t> data;
        if (!ReadFile(patch.path, data) ||
            !(patch.bps ? MsrPatch::ApplyBps(data, buffer) : MsrPatch::ApplyIps(data, buffer))) {
            std::fprintf(stderr, "msr: %s does not fit the game's code\n", patch.path.c_str());
            return -1;
        }
        std::fprintf(stderr, "msr: code patched by %s\n", patch.path.c_str());
        used = buffer.size();   // a patch may write into the .bss pages too
        changed = true;
    }
    if (!changed) return 0;
    auto* out = static_cast<std::uint8_t*>(std::malloc(buffer.size()));
    if (!out) return -1;
    std::memcpy(out, buffer.data(), buffer.size());
    std::free(*code);
    *code = out;
    *size = static_cast<uint32_t>(used);
    return 1;
}

extern "C" void msr_mods_init(const char* user, const char* settings, const char* exe) {
    NativeMods::Init(user ? user : "", settings ? settings : "", exe ? exe : "");
}

extern "C" void msr_mods_boot(int packs_given) {
    NativeMods::Boot(packs_given != 0);
}
