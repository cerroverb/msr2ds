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
#include <vector>
#include <QString>
#include "msr/overlay_hook.h"

struct ImGuiContext;

namespace MSR {

/// Options of the single-screen presentation, saved in user/msr/settings.ini.
struct PlayerSettings {
    bool widescreen = true;
    float camera_zoom = 1.f; ///< The view in play: below 1 wider, above 1 closer
    float hud_scale = .6f;
    float hud_opacity = 1.f;
    bool show_minimap = true;
    float minimap_size = 1.15f;
    float minimap_opacity = .85f;
    bool show_counters = true;
    bool show_abilities = true;
    float panel_scale = 1.f;
    float menu_backdrop = .75f; ///< Opacity of the card behind the title menus
    bool control_hints = true;  ///< Help text beside the pause menu, file select and touch mode
    float menu_scale = 1.34f;
    bool show_fps = false;
    int frame_rate = 60; ///< One of FrameRate::Choices (msr/frame_rate.h)
    /// macOS: shaders built in the background (async_shader_compilation, msr/defaults.cpp), so the
    /// first sight of an effect does not pause the game while Metal compiles it; the effect is
    /// missing for those frames instead (flicker).
    bool background_shaders = false;
    // Extras otherwise given by amiibo (msr/cheats.h)
    bool unlock_galleries = true;
    bool unlock_fusion_mode = true;
    bool fusion_suit = false;
    // Cheats: inventory items kept at their maximum (msr/cheats.h Refills)
    bool infinite_energy = false;
    bool infinite_missiles = false; ///< Super Missiles and Power Bombs too
    bool infinite_aeion = false;

    void Load(const QString& path);
    void Save(const QString& path) const;
    /// Pushes the values to the renderer (msr/presentation.h).
    void Publish() const;
};

/// Camera zoom range: the widest and the closest view in play (1 is the game's).
inline constexpr float CameraZoomMin = .7f;
inline constexpr float CameraZoomMax = 1.5f;

/// Frame rate choices, as FrameRate::Choices.
inline constexpr std::array<const char*, 4> FrameRateNames = {"30 FPS (original)", "60 FPS",
                                                              "120 FPS", "144 FPS"};

/// Internal resolution choices, by factor (0 = match the window).
inline constexpr std::array<const char*, 11> ResolutionNames = {
    "Match window", "1x (240p)",  "2x (480p)",  "3x (720p)",      "4x (960p)", "5x (1200p)",
    "6x (1440p)",   "7x (1680p)", "8x (1920p)", "9x (2160p, 4K)", "10x (2400p)"};

/// Emulator options shown in the overlay. Read and applied on the Qt thread.
struct EmulatorSettings {
    bool fullscreen = false;
    int resolution = 9; ///< Internal resolution factor, 0 = match the window
    bool vsync = true;
    int volume = 100;   ///< Percent
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
    };
    Kind kind;
    EmulatorSettings emulator{};
    int value = 0;
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

private:
    struct Event {
        enum class Type { Key, Analog, Position, Button, Wheel } type;
        int code;
        bool down;
        float x, y;
    };

    void Push(const Event& event);
    void Act(OverlayAction::Kind kind, int value = 0);
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
    void PlayerChanged(bool layout);
    void EmulatorChanged();

    std::atomic<bool> visible{false};
    std::atomic<double> hint_started{-1.0};

    std::mutex mutex; ///< Guards the queues and the settings snapshots below
    std::vector<Event> events;
    std::vector<OverlayAction> actions;
    PlayerSettings player;
    EmulatorSettings emulator;
    ControlsSnapshot controls;
    ModsSnapshot mods;

    // Renderer thread only
    ImGuiContext* context{};
    bool backend_ready = false;
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
};

/// The overlay registered with the renderer, created by the frontend.
Overlay& GetOverlay();

} // namespace MSR
