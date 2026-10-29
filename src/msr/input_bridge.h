// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <QObject>
#include "core/frontend/input.h"
#include "msr/touch_nav.h"

class QWidget;

namespace MSR {

/// Routes player input outside the game (Qt thread):
///  - F1 or Select (as bound in the input profile): the settings overlay; while it is open,
///    keyboard, mouse and gamepad drive it. The game treats Select as Start, so it never
///    receives it (hid.cpp);
///  - Tab, or L3 while a menu is on screen: touch navigation (D-pad or left stick move, A taps,
///    B leaves). In play the stick clicks do nothing here, so a stray one opens nothing;
///  - ZR and ZL (the triggers), which the game leaves unused, for what it otherwise offers only
///    by touch: ZR steps through the beam slots (the usual beam, the Ice Beam, the Grapple
///    Beam), ZL rolls into the Morph Ball and back, as in Metroid Dread.
class InputBridge final : public QObject {
public:
    /// `before_open` refreshes the emulator settings shown by the menu.
    InputBridge(QWidget* render_window, std::function<void()> before_open, QObject* parent);
    ~InputBridge() override;

    /// Polls the gamepads; call about every 16 ms.
    void Poll();

    /// Any gamepad button is held (as of the last poll).
    bool AnyPadHeld() const {
        return any_held;
    }
    /// While set, gamepad input is not sent to the menu, and key presses go to `capture`
    /// (return true to consume). Used to rebind controls.
    void SetCapture(std::function<bool(int key)> capture);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void ToggleOverlay();
    void ToggleTouch();
    bool OverRenderWindow(QObject* watched) const;
    /// The game is on screen (not the launcher or the loading screen).
    bool InGame() const;
    /// Follows the input profile's Select binding.
    void UpdateSelectBinding();
    /// Taps the next beam slot Samus has (Presentation::beam_slot).
    void CycleBeam(std::chrono::steady_clock::time_point time);
    /// Taps the HUD's map, which rolls Samus into the Morph Ball or stands her up.
    void ToggleMorphBall(std::chrono::steady_clock::time_point time);

    /// A 3DS button the game leaves unused, as the input profile binds it.
    struct SpareButton {
        int native; ///< Settings::NativeButton value
        std::string binding;
        std::unique_ptr<Input::ButtonDevice> device;
        bool previous = true;
        /// Pressed since the last call; a button held when its binding changes waits for release.
        bool Pressed();
    };

    static constexpr int ButtonCount = 15; // SDL_GameControllerButton A .. DPAD_RIGHT
    QWidget* render_window;
    std::function<void()> before_open;
    std::function<bool(int key)> capture;
    std::array<std::unique_ptr<Input::ButtonDevice>, ButtonCount> buttons;
    std::unique_ptr<Input::AnalogDevice> stick;
    std::string select_binding;
    std::unique_ptr<Input::ButtonDevice> select_button;
    int select_key = -1; ///< Qt key of a keyboard Select binding, or -1
    bool select_previous = false;
    SpareButton zr;
    SpareButton zl;
    int beam_target = -1; ///< The slot last tapped, until the bottom screen shows it
    std::chrono::steady_clock::time_point beam_settled{};
    std::array<bool, ButtonCount> previous{};
    float previous_x = 0.f, previous_y = 0.f;
    bool was_visible = false;
    bool any_held = false;

    TouchNavigator touch;
    std::chrono::steady_clock::time_point last_poll{};
    int stick_direction_x = 0, stick_direction_y = 0;
    std::chrono::steady_clock::time_point stick_repeat{};
};

} // namespace MSR
