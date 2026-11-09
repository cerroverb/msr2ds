// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// In-game settings overlay, drawn by the renderer with Dear ImGui (the DekUI approach). F1 or
// L3+R3 opens it; it is navigable with a gamepad, the keyboard or the mouse, and the game keeps
// running behind it without receiving input. Input and resulting actions cross threads through
// small queues: the Qt thread feeds events and applies emulator changes, the renderer draws.

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef MSR_NATIVE
#include <QString>
#endif
#include "msr/overlay_hook.h"

struct ImGuiContext;

namespace MSR {

/// Options of the single-screen presentation, saved in user/msr/settings.ini.
struct PlayerSettings {
    bool widescreen = true;
    bool ultrawide = true;   ///< With widescreen: the view fills a window wider than 16:9, up to 32:9 (never on without it)
    bool fill_16_10 = true;  ///< With widescreen: the view fills a window taller than 16:9, down to 16:10 (never on without it)
    float camera_zoom = 1.f; ///< The view in play: below 1 wider, above 1 closer
    bool wide_culling = true; ///< msr-native: the game draws all of a wider view (Presentation::wide_culling)
    float hud_scale = .6f;
    float hud_opacity = 1.f;
    bool show_minimap = true;
    float minimap_size = 1.15f;
    float minimap_opacity = .85f;
    bool show_counters = true;
    bool show_abilities = true;
    bool hud_within_16_9 = false; ///< Ultrawide: the HUD at the corners of the view's middle 16:9
    float panel_scale = 1.f;
    float menu_backdrop = .75f; ///< Opacity of the card behind the title menus
    bool control_hints = true;  ///< Help text beside the pause menu, file select and touch mode
    float menu_scale = 1.34f;
    bool show_fps = false;
    int frame_rate = 60; ///< One of FrameRate::Choices (msr/frame_rate.h)
    // msr-native's picture (EmulatorSettings::bloom_option, hdr_option)
    float bloom = .75f;    ///< The game's glow, 1 = as the game draws it
    bool hdr = false;      ///< HDR output
    float hdr_peak = 0.f;  ///< HDR: the brightest highlights in nits, 0 = the display's peak
    float hdr_paper = 0.f; ///< HDR: the game's white in nits, 0 = Windows' SDR content brightness
    // Extras otherwise given by amiibo (msr/cheats.h)
    bool unlock_galleries = true;
    bool unlock_fusion_mode = true;
    bool unlock_hard_mode = true;  ///< Hard Mode from the start: a new file and every New Game offer it
    bool fusion_suit = false;
    // Cheats: inventory items kept at their maximum (msr/cheats.h Refills)
    bool infinite_energy = false;
    bool infinite_missiles = false; ///< Super Missiles and Power Bombs too
    bool infinite_aeion = false;
    int language = 1; ///< The game's text: a 3DS system language, one of Languages (msr-native)
    // The video settings the player last set by hand, which no preset makes (the Display tab's Custom): kept when a
    // preset replaces them, so Custom brings them back
    bool custom_video = false; ///< The values below are the player's (none yet: Custom is not offered)
    int custom_resolution = 4;
    int custom_aniso = 1;
    bool custom_fsr = true;
    int custom_frame_rate = 60;
    bool custom_wide_culling = true;
    bool custom_power_saving = false;

#ifdef MSR_NATIVE
    // msr-native reads and writes settings.ini without Qt (native/runtime/msr/settings.cpp)
    void Load(const std::string& path);
    void Save(const std::string& path) const;
#else
    void Load(const QString& path);
    void Save(const QString& path) const;
#endif
    /// Pushes the values to the renderer (msr/presentation.h).
    void Publish() const;
};

/// The languages the North American game has text for, by 3DS system language (cfg block 0x000A0002). The game keeps
/// the system language only where the console's region has it (0x1007D6): German, Italian and Dutch need Europe, which
/// msr-native's cfg service then reports (native/runtime/services.c), as the emulator-based 1.x did.
struct Language {
    const char* name;
    int value;
    bool european;
};
inline constexpr std::array<Language, 6> Languages{{
    {"English", 1, false},
    {"Fran\xC3\xA7" "ais", 2, false},
    {"Espa\xC3\xB1" "ol", 5, false},
    {"Deutsch", 3, true},
    {"Italiano", 4, true},
    {"Nederlands", 8, true},
}};

/// Camera zoom range: the widest and the closest view in play (1 is the game's).
inline constexpr float CameraZoomMin = .7f;
inline constexpr float CameraZoomMax = 1.5f;

/// Frame rate choices, as FrameRate::Choices.
inline constexpr std::array<const char*, 5> FrameRateNames = {"30 FPS (original)", "60 FPS",
                                                              "90 FPS", "120 FPS", "144 FPS"};

/// Internal resolution choices, by factor (0 = match the window).
inline constexpr std::array<const char*, 11> ResolutionNames = {
    "Match window", "1x (240p)",  "2x (480p)",  "3x (720p)",      "4x (960p)", "5x (1200p)",
    "6x (1440p)",   "7x (1680p)", "8x (1920p)", "9x (2160p, 4K)", "10x (2400p)"};

/// Anisotropic filtering choices (EmulatorSettings::aniso), as video.c takes them, and their names.
inline constexpr std::array<int, 5> AnisoChoices{1, 2, 4, 8, 16};
inline constexpr std::array<const char*, 5> AnisoNames{"Off", "2x", "4x", "8x", "16x"};

/// The largest anisotropic filtering choice up to `value` (settings.ini may hold anything).
constexpr int AnisoChoice(int value) {
    int choice = AnisoChoices.front();
    for (const int c : AnisoChoices) {
        if (c <= value) choice = c;
    }
    return choice;
}

/// The Display tab's video presets: the internal resolution, anisotropic filtering, upscaling, frame rate and how much
/// of a wider view the game draws, set together; Minimum also turns power-saving waits on. Other values are Custom,
/// which the menu keeps (PlayerSettings::custom_video) while a preset is in use. Bloom only scales the game's glow, and
/// V-Sync and HDR follow the display: presets leave them as they are.
struct VideoPreset {
    const char* name;
    int resolution;     ///< Internal resolution factor (at most EmulatorSettings::max_resolution)
    int aniso;          ///< One of AnisoChoices
    bool fsr;           ///< Upscaling with FSR 1, else linear
    int frame_rate;     ///< 30: the original rate; above: at least this, so a faster rate chosen for a faster display stays
    bool only_3ds_view; ///< The game draws only what the 3DS's view shows (PlayerSettings::wide_culling off), as in 1.x
    bool power_saving;  ///< Turns power-saving waits on; false leaves them as they are (they follow the processor)
};
inline constexpr std::array<VideoPreset, 4> VideoPresets{{
    {"Minimum", 2, 1, false, 30, true, true},
    {"Medium", 4, 1, true, 60, false, false}, // the launcher's defaults
    {"High", 6, 8, true, 60, false, false},
    {"Ultra", 9, 16, true, 60, false, false},
}};

/// Power-saving waits by default: on a processor with 4 threads or fewer, where the spinning waits take the game's.
inline bool PowerSavingDefault() {
    const unsigned threads = std::thread::hardware_concurrency();
    return threads != 0 && threads <= 4;
}

/// Emulator options shown in the overlay. Read and applied on the Qt thread.
struct EmulatorSettings {
    bool fullscreen = false;
    int resolution = 9; ///< Internal resolution factor, 0 = match the window
    bool vsync = true;
    int volume = 100;   ///< Percent
    int max_resolution = 10; ///< The largest factor offered
    bool quick_saves = true; ///< Quick save and load
    bool bloom_option = false; ///< The Bloom slider (msr-native)
    bool ultrawide_option = false; ///< The Ultrawide and HUD within 16:9 rows (msr-native)
    bool deck_preset = false;  ///< The Steam Deck preset button (msr-native on a Steam Deck)
    int aniso = 1;             ///< Anisotropic filtering, one of AnisoChoices (1 = off)
    bool aniso_option = false; ///< The Anisotropic filtering row, and the video presets set it (msr-native)
    // msr-native, for slower computers (performance_options): the Upscaling row (the video presets set it too) and the
    // Power-saving waits row
    bool performance_options = false;
    bool fsr = true;           ///< Upscaling with FSR 1, else linear (lighter)
    bool power_saving = false; ///< Waits sleep instead of spinning (msr-native main.c main_set_power_saving)
    int texture_detail = 1;    ///< Texture pack size: 1 full, 2 half, 4 quarter (msr-native textures.cpp), from the next start
    int texture_detail_running = 1; ///< What the running game loads packs at (in the launcher: the same)
    // msr-native with OpenGL (MSR_OPENGL): the Graphics API row. 0 automatic (Vulkan with 1.3, else OpenGL), 1 Vulkan,
    // 2 OpenGL, from the next start; settings.ini [native] renderer
    bool renderer_option = false;
    int renderer = 0;
    int renderer_running = 0; ///< The choice the running game started with (in the launcher: the same)
    int renderer_active = 0;  ///< What presents now: 1 Vulkan, 2 OpenGL (0 not known yet)
    int hdr_option = 0;        ///< The HDR rows: 0 none (the emulator build), 1 the display is not in HDR mode, 2 HDR possible
    float display_peak = 1000.f, display_paper = 200.f; ///< The display's peak brightness, Windows' SDR white (nits)
};

/// The controls shown in the overlay's Controls tab. Built and applied on the Qt thread.
struct ControlsSnapshot {
    struct Row {
        int id;              ///< Settings::NativeButton value, or 100 + NativeAnalog value
        std::string label;   ///< The 3DS control
        std::string binding; ///< What it is bound to
    };
    std::string profile;
    std::vector<Row> rows;
    int capturing = -1; ///< Row waiting for a new input, or -1
    bool controller_connected = false;
    bool right_stick_option = false; ///< msr-native: the Aeion abilities can be put on the right stick
    bool right_stick_dpad = false;   ///< The right stick is the D-pad, the pad's D-pad the Circle Pad
    bool menu_layout_option = false; ///< msr-native: menus can confirm as the game does (Nintendo menu layout)
    bool menu_confirm_right = false; ///< Menus choose with the game's A as bound (else A / Cross, the bottom button)
    bool trigger_option = false;     ///< msr-native: the triggers are buttons with a press point (#35)
    int trigger_press = 50;          ///< How far a trigger is pulled to press it, % of its travel (10..90)
};

/// The Mods and Textures tabs: game mods and texture packs (msr/mods.h). Built and applied on
/// the Qt thread.
struct ModsSnapshot {
    struct Entry {
        std::string name;
        std::string detail; ///< What a mod changes; a pack's texture count and pack.json details
        bool enabled;
    };
    std::vector<Entry> mods; ///< Game mods, highest priority first
    std::string mod_folder;
    std::vector<Entry> packs; ///< Texture packs, highest priority first
    std::string folder;
    bool preload = false;
    bool dump = false;
    bool restart_needed = false; ///< Changed since the game booted
    bool texture_options = true; ///< Preload and dump can be changed (msr-native always preloads, never dumps)
};

/// msr-native's launcher: the menu before the game runs (native/runtime/msr/launcher.cpp). Its
/// Game tab starts with Play, the game file and the native code (the game's code recompiled from
/// the player's dump). Built and applied on the launcher's thread.
struct LauncherSnapshot {
    enum class Code { Missing, Ready, Outdated, Building, Failed };
    bool active = false;     ///< The menu is the launcher (no game running)
    std::string version;     ///< "msr-native 0.1.0"
    std::string game;        ///< The dump's path, empty when none was chosen
    std::string game_note;   ///< What is wrong with it, or empty
    Code code = Code::Missing;
    std::string code_note;   ///< When it was built, the build's step, or what failed
    float progress = -1.f;   ///< The build's progress, 0..1, or -1 when unknown
    bool can_play = false;
    bool can_browse = false; ///< A file dialog is there
    std::string folder;      ///< The user folder (saves, settings, mods)
    /// A newer release (launcher.cpp looks when it starts): offered, being put in place, or failed
    enum class Update { None, Available, Working, Failed };
    Update update = Update::None;
    std::string update_version;      ///< "0.2.0"
    std::string update_note;         ///< Its size, the download's progress, or what failed
    float update_progress = -1.f;    ///< The download's progress, 0..1, or -1
    bool update_installable = false; ///< The launcher can put it in place (else: its download page)
    /// The program beside the launcher that starts the game without it (msr-native-direct.exe), once
    /// the native code is built; empty when there is none
    std::string direct;
    /// The native code can be built here (not in msr-native's Android app: a PC builds it there)
    bool can_build = true;
    /// msr-native on a PC: the Android app's native code can be built too (Build for Android)
    bool android_build = false;
    std::string android_note; ///< The Android build's result: where to copy it, or what failed
    /// Save import (launcher.cpp, the saves section): the saves of a 3DS backup or another emulator,
    /// looked for under a folder the player chooses, then put in place of the ones here once confirmed
    enum class Saves { Idle, Searching, Confirm, Done, Failed };
    Saves saves = Saves::Idle;
    std::string saves_from;   ///< Confirm: the folder holding the saves found
    std::string saves_detail; ///< Confirm: how many files, when last saved, other save folders there
    std::string saves_note;   ///< Searching: where; Done: where the previous saves went; Failed: why
    std::string saves_folder; ///< Where the game keeps its saves
};

struct OverlayAction {
    enum class Kind {
        Emulator,         ///< Apply `emulator`
        Player,           ///< The menu closed: save the player settings
        Layout,           ///< Widescreen or the panel size changed: recompute the screen layout
        QuickSave,
        QuickLoad,
        Restart,
        Quit,
        Bind,             ///< Wait for a new input for control `value`
        CancelBind,
        GamepadLayout,    ///< Map every control to the connected gamepad, by position
        KeyboardLayout,   ///< Restore the default keyboard bindings
        RightStickDpad,   ///< msr-native: the Aeion abilities on the right stick (`value` 0 or 1)
        MenuConfirmRight, ///< msr-native: the Nintendo menu layout (`value` 0 or 1)
        TriggerPress,     ///< msr-native: the triggers' press point (`value` %)
        FrameRate,        ///< The frame rate changed: apply the CPU clock and presentation
        GameMod,          ///< Enable or disable game mod `value` (an index in ModsSnapshot)
        GameModUp,        ///< Raise game mod `value`'s priority
        GameModDown,      ///< Lower game mod `value`'s priority
        GameModsFolder,   ///< Open the game mods folder
        TexturePack,      ///< Enable or disable texture pack `value` (an index in ModsSnapshot)
        TexturePackUp,    ///< Raise texture pack `value`'s priority
        TexturePackDown,  ///< Lower texture pack `value`'s priority
        ModsFolder,       ///< Open the texture packs folder
        PreloadTextures,  ///< Load every pack texture at boot (`value` 0 or 1)
        DumpTextures,     ///< Save the game's textures for making packs (`value` 0 or 1)
        LauncherPlay,     ///< Start the game
        LauncherBuild,    ///< Build the native code from the game file
        LauncherBuildAndroid, ///< msr-native: build the Android app's native code (on a PC)
        LauncherCancel,   ///< Stop the build
        LauncherBrowse,   ///< Choose the game file with a file dialog
        LauncherGame,     ///< The game file typed in: `text`
        LauncherFolder,   ///< Open the user folder
        LauncherUpdate,   ///< Download the newer release and put it in place
        LauncherUpdatePage, ///< Open the newer release's page (what's new; its download)
        LauncherImportSaves,   ///< Choose a folder with saves to import (a folder dialog, then a search)
        LauncherImportConfirm, ///< Import the saves found
        LauncherImportCancel,  ///< Leave the saves found
        LauncherSavesFolder,   ///< Open the saves folder
    };
    Kind kind;
    EmulatorSettings emulator{};
    int value = 0;
    std::string text{};
};

class Overlay final : public OverlayHook::Interface {
public:
    Overlay();
    ~Overlay() override;

    // Qt thread
    void SetVisible(bool visible);
    bool Visible() const {
        return visible.load();
    }
    void Toggle() {
        SetVisible(!Visible());
    }
    void ShowStartupHint();
    void SetEmulatorSettings(const EmulatorSettings& settings);
    void SetPlayerSettings(const PlayerSettings& settings);
    void SetControls(const ControlsSnapshot& controls);
    void SetMods(const ModsSnapshot& mods);
    /// msr-native: the launcher's state; while it is active the menu stays open.
    void SetLauncher(const LauncherSnapshot& launcher);
    bool LauncherActive() const {
        return launcher_active.load();
    }
    PlayerSettings GetPlayerSettings();
    /// ImGui key events (ImGuiKey values), mouse in framebuffer pixels.
    void Key(int key, bool down);
    void Analog(int key, bool down, float value);
    void MousePosition(float x, float y);
    void MouseButton(int button, bool down);
    void MouseWheel(float delta);
    std::vector<OverlayAction> TakeActions();

    // Renderer (OverlayHook::Interface)
    void Init(const OverlayHook::VulkanContext& context) override;
    void Draw(VkCommandBuffer command, std::uint32_t width, std::uint32_t height) override;
    void Shutdown() override;
    /// msr-native's OpenGL presenter (MSR_OPENGL): the same menu through Dear ImGui's OpenGL 3 backend, with the
    /// context current; DrawOpenGL draws on the bound framebuffer.
    void InitOpenGL();
    void DrawOpenGL(std::uint32_t width, std::uint32_t height);
    /// msr-native: the menu shows the Mods or Textures tab, so their folders are scanned again for what the player
    /// added or removed meanwhile. Renderer thread.
    bool ModsTabShown() const;

private:
    struct Event {
        enum class Type { Key, Analog, Position, Button, Wheel } type;
        int code;
        bool down;
        float x, y;
    };

    void Push(const Event& event);
    void Act(OverlayAction::Kind kind, int value = 0, std::string text = {});
    /// The launcher's part of the Game tab: Play, the game file, the native code.
    void DrawLauncher(const LauncherSnapshot& launcher, float label_width,
                      const std::function<void()>& default_focus);
    void DrawControls(float scale, float label_width, const std::function<void()>& default_focus);
    void DrawGameMods(const std::function<void()>& default_focus);
    void DrawTexturePacks(float label_width, const std::function<void()>& default_focus);
    /// The rows of a mod list: enable each one, move it up or down. Returns how many there are.
    int DrawModList(const ModsSnapshot& snapshot,
                    std::vector<ModsSnapshot::Entry> ModsSnapshot::*list, OverlayAction::Kind toggle,
                    OverlayAction::Kind up, OverlayAction::Kind down,
                    const std::function<void()>& default_focus);
    /// A Mods change shows at once; the Qt thread applies it and sends a fresh snapshot.
    void ChangeMods(OverlayAction::Kind kind, int value,
                    const std::function<void(ModsSnapshot&)>& edit);
    /// Below a mod list: the restart offer after a change, else `note`.
    void DrawModsFooter(const ModsSnapshot& snapshot, const char* note);
    ModsSnapshot ModsCopy();
    /// Beside the pause menu and file select: the controls the game only offers by touch, and
    /// how to reach them with touch mode; in touch mode, its own controls.
    void DrawControlHints(float width, float height);
    void DrawMenu(float scale, bool appearing);
    void DrawHint(float scale, float width, float height);
    void DrawFps(float scale, float width);
    /// One frame of the menu up to ImGui::Render, after the backend's `new_frame`; the backend then draws it.
    void BuildFrame(std::uint32_t width, std::uint32_t height, void (*new_frame)());
    void PlayerChanged(bool layout);
    void EmulatorChanged();

    std::atomic<bool> visible{false};
    std::atomic<bool> launcher_active{false};
    std::atomic<double> hint_started{-1.0};

    std::mutex mutex; ///< Guards the queues and the settings snapshots below
    std::vector<Event> events;
    std::vector<OverlayAction> actions;
    PlayerSettings player;
    EmulatorSettings emulator;
    ControlsSnapshot controls;
    ModsSnapshot mods;
    LauncherSnapshot launcher;

    // Renderer thread only
    ImGuiContext* context{};
    bool backend_ready = false;
    bool opengl = false; ///< The backend is OpenGL's (InitOpenGL), not Vulkan's
    float font_size = 32.f;
    double previous_time = 0.0;
    int tab = 0;
    int requested_tab = -1;
    bool was_visible = false;
    float fps = 0.f;
    int hint_set = -1;      ///< Control hints shown (or fading out), an index in the hint sets
    float hint_alpha = 0.f;
    std::uint64_t fps_frames = 0; ///< Game frames when the count started
    double fps_started = 0.0;
    std::array<char, 1024> game_input{}; ///< The launcher's game file field
    std::string game_input_from;         ///< The path it was last filled from
};

/// The overlay registered with the renderer, created by the frontend.
Overlay& GetOverlay();

} // namespace MSR
