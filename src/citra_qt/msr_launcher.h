// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <QWidget>
#include "core/frontend/input.h"

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSlider;
class QTabWidget;
class QTimer;

namespace InputCommon::Polling {
class DevicePoller;
}

namespace MSR {

struct PlayerSettings;

/// The launcher, shown in the main window before the game boots: the game dump to play (the
/// package ships without it) and the same settings as the in-game menu, saved to the same
/// places. Mouse, keyboard and gamepad all drive it: D-pad or stick up and down move between
/// options, left and right change them, A activates, LB and RB switch tabs, Start plays.
class Launcher final : public QWidget {
    Q_OBJECT

public:
    explicit Launcher(QWidget* parent);
    ~Launcher() override;

    /// Shows the launcher with the current settings and the dump that will boot.
    void Open();
    /// Uses a dump chosen by the player (browsed for, or dropped onto the window).
    void ChooseRom(const QString& path);

signals:
    void PlayRequested();
    void QuitRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct BindRow {
        int id;
        QPushButton* button;
    };

    QWidget* BuildDisplayTab();
    QWidget* BuildHudTab();
    QWidget* BuildControlsTab();
    QWidget* BuildGameTab();
    QWidget* BuildModsTab();
    QWidget* BuildCheatsTab();
    /// Rebuilds the texture pack rows from disk.
    void RefreshMods();
    /// Up and down (and Tab) walk the options top to bottom, mod rows included.
    void UpdateFocusOrder();
    QCheckBox* AddCheck(QGridLayout* grid, const QString& label,
                        std::function<void(bool)> changed);
    QSlider* AddSlider(QGridLayout* grid, const QString& label, int min, int max,
                       std::function<QString(int)> format, std::function<void(int)> changed);
    void EditPlayer(const std::function<void(PlayerSettings&)>& edit);

    void Load();
    void UpdateRom(const QString& error = {});
    void Browse();
    /// Replaces this game's saves with ones from a folder the player picks (msr/saves.h).
    void ImportSaves();
    void RefreshControls();
    void StartBind(int id);
    void FinishBind(const std::string& param);
    void StopBind();

    void Poll();
    void Navigate(bool forward);
    void Adjust(int direction);
    void Activate();
    void SwitchTab(int direction);
    void FollowFocus();

    QLabel* icon = nullptr;
    QLineEdit* rom_path = nullptr;
    QLabel* rom_status = nullptr;
    QPushButton* browse = nullptr;
    QTabWidget* tabs = nullptr;
    QPushButton* play = nullptr;
    QPushButton* quit = nullptr;
    QWidget* focus_ring = nullptr;

    // Settings widgets, filled by Load()
    QCheckBox* fullscreen = nullptr;
    QCheckBox* widescreen = nullptr;
    QComboBox* resolution = nullptr;
    QCheckBox* vsync = nullptr;
    QCheckBox* show_fps = nullptr;
    QSlider* menu_size = nullptr;
    QSlider* hud_size = nullptr;
    QSlider* hud_opacity = nullptr;
    QCheckBox* show_map = nullptr;
    QSlider* map_size = nullptr;
    QSlider* map_opacity = nullptr;
    QCheckBox* show_counters = nullptr;
    QCheckBox* show_abilities = nullptr;
    QSlider* panel_size = nullptr;
    QSlider* menu_backdrop = nullptr;
    QCheckBox* control_hints = nullptr;
    QSlider* volume = nullptr;
    QCheckBox* unlock_galleries = nullptr;
    QCheckBox* unlock_fusion_mode = nullptr;
    QCheckBox* fusion_suit = nullptr;
    QCheckBox* infinite_energy = nullptr;
    QCheckBox* infinite_missiles = nullptr;
    QCheckBox* infinite_aeion = nullptr;
    QPushButton* import_saves = nullptr;
    QPushButton* open_saves = nullptr;
    QComboBox* frame_rate = nullptr;
    QWidget* pack_list = nullptr;
    QScrollArea* packs = nullptr;
    QPushButton* open_mods = nullptr;
    QPushButton* refresh_mods = nullptr;
    QCheckBox* preload_textures = nullptr;
    QCheckBox* dump_textures = nullptr;
    std::vector<QWidget*> pack_widgets; ///< Focusable widgets of the pack rows, in order
    QLabel* profile = nullptr;
    QPushButton* gamepad_layout = nullptr;
    QPushButton* keyboard_layout = nullptr;
    QScrollArea* bindings = nullptr;
    std::vector<BindRow> bind_rows;

    // Rebinding: the row waiting for an input, the gamepad pollers once the pad is released.
    int bind_id = -1;
    bool bind_started = false;
    std::chrono::steady_clock::time_point bind_deadline{};
    std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> pollers;

    // Gamepad navigation
    static constexpr int ButtonCount = 15; // SDL_GameControllerButton A .. DPAD_RIGHT
    QTimer* poll_timer = nullptr;
    std::array<std::unique_ptr<Input::ButtonDevice>, ButtonCount> buttons;
    std::unique_ptr<Input::AnalogDevice> stick;
    std::array<bool, ButtonCount> previous{};
    int stick_x = 0, stick_y = 0;
    std::chrono::steady_clock::time_point stick_repeat{};
    std::chrono::steady_clock::time_point next_refresh{};
};

} // namespace MSR
