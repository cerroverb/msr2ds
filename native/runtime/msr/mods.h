// Game mods and texture packs on msr-native, found and ordered as the launcher and the emulator-based build do
// (src/msr/mods.cpp): <user>/mods/game and <user>/mods/textures, the order and the disabled ones in settings.ini
// [game_mods] and [texture_packs].
#pragma once

#include <string>
#include <vector>

namespace NativeMods {

struct GameMod {
    std::string name;   ///< Folder in the game mods folder
    std::string path;   ///< Its files' folder, ending in '/'; empty when it holds none
    bool enabled = true;
};

struct TexturePack {
    std::string name;   ///< Folder, relative to the packs folder
    std::string path;   ///< Absolute folder path, ending in '/'
    bool enabled = true;
    bool built_in = false;
};

/// The user folder (ending in '/'), settings.ini and the folder of msr-native's executable.
void Init(const std::string& user_folder, const std::string& settings_file, const std::string& exe_folder);

std::string GameModFolder();
std::vector<GameMod> GameMods();
std::string Describe(const GameMod& mod);
bool IsRandomizer(const GameMod& mod);
void SetModEnabled(const std::string& name, bool enabled);
void MoveMod(const std::string& name, int direction);

std::string TexturePackFolder();
std::vector<TexturePack> TexturePacks();
std::string Describe(const TexturePack& pack);
void SetPackEnabled(const std::string& name, bool enabled);
void MovePack(const std::string& name, int direction);

/// What a restart would load: compared with Booted() to tell whether a change needs one.
std::string Pending();
/// At start-up: the enabled game mods' folders (highest priority first) for the RomFS and the code, Cheats::randomizer,
/// and the enabled texture packs unless `packs_given` (--texture-pack). Remembers Pending().
void Boot(bool packs_given);
const std::vector<std::string>& BootedMods();
const std::string& Booted();

/// Opens a folder in the file manager.
void OpenFolder(const std::string& path);

} // namespace NativeMods
