// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <cmath>
#include <tuple>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QWheelEvent>
#include <QWidget>
#include <imgui.h>
#include "common/param_package.h"
#include "common/settings.h"
#include "msr/bottom_classifier.h"
#include "msr/input_bridge.h"
#include "msr/overlay.h"
#include "msr/presentation.h"

namespace MSR {
namespace {

// SDL_GameControllerButton order: A, B, X, Y, BACK, GUIDE, START, LEFTSTICK, RIGHTSTICK,
// LEFTSHOULDER, RIGHTSHOULDER, DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT. SDL's A and B are the
// bottom and right face buttons; the game, and the menus with it, take the right one as its A.
constexpr int ButtonA = 0;
constexpr int ButtonB = 1;
constexpr int Choose = ButtonB;
constexpr int Back = ButtonA;
constexpr int LeftStick = 7;
constexpr int DpadUp = 11;
constexpr int DpadDown = 12;
constexpr int DpadLeft = 13;
constexpr int DpadRight = 14;
constexpr std::array<ImGuiKey, 15> GamepadKeys = {
    ImGuiKey_GamepadFaceDown,  ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft,
    ImGuiKey_GamepadFaceUp,    ImGuiKey_GamepadBack,      ImGuiKey_None,
    ImGuiKey_GamepadStart,     ImGuiKey_GamepadL3,        ImGuiKey_GamepadR3,
    ImGuiKey_GamepadL1,        ImGuiKey_GamepadR1,        ImGuiKey_GamepadDpadUp,
    ImGuiKey_GamepadDpadDown,  ImGuiKey_GamepadDpadLeft,  ImGuiKey_GamepadDpadRight,
};

ImGuiKey MapKey(int key) {
    switch (key) {
    case Qt::Key_Up:
        return ImGuiKey_UpArrow;
    case Qt::Key_Down:
        return ImGuiKey_DownArrow;
    case Qt::Key_Left:
        return ImGuiKey_LeftArrow;
    case Qt::Key_Right:
        return ImGuiKey_RightArrow;
    case Qt::Key_Return:
        return ImGuiKey_Enter;
    case Qt::Key_Enter:
        return ImGuiKey_KeypadEnter;
    case Qt::Key_Space:
        return ImGuiKey_Space;
    case Qt::Key_Escape:
        return ImGuiKey_Escape;
    case Qt::Key_Tab:
        return ImGuiKey_Tab;
    case Qt::Key_PageUp:
        return ImGuiKey_PageUp;
    case Qt::Key_PageDown:
        return ImGuiKey_PageDown;
    case Qt::Key_Home:
        return ImGuiKey_Home;
    case Qt::Key_End:
        return ImGuiKey_End;
    case Qt::Key_Shift:
        return ImGuiKey_LeftShift;
    case Qt::Key_Control:
        return ImGuiKey_LeftCtrl;
    default:
        return ImGuiKey_None;
    }
}

long long SteadyNanoseconds(std::chrono::steady_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

} // namespace

InputBridge::InputBridge(QWidget* render_window_, std::function<void()> before_open_,
                         QObject* parent)
    : QObject(parent), render_window(render_window_), before_open(std::move(before_open_)) {
    zr.native = Settings::NativeButton::ZR;
    zl.native = Settings::NativeButton::ZL;
    // "maptype all" reads the button from any connected game controller.
    for (int i = 0; i < ButtonCount; ++i) {
        Common::ParamPackage params({{"engine", "sdl"}, {"api", "controller"}, {"maptype", "all"}});
        params.Set("button", i);
        buttons[i] = Input::CreateDevice<Input::ButtonDevice>(params.Serialize());
    }
    Common::ParamPackage params({{"engine", "sdl"}, {"api", "controller"}, {"maptype", "all"}});
    params.Set("axis_x", 0);
    params.Set("axis_y", 1);
    params.Set("deadzone", 0.25f);
    stick = Input::CreateDevice<Input::AnalogDevice>(params.Serialize());
    last_poll = std::chrono::steady_clock::now();
}

InputBridge::~InputBridge() = default;

void InputBridge::SetCapture(std::function<bool(int key)> capture_) {
    capture = std::move(capture_);
}

void InputBridge::ToggleOverlay() {
    auto& overlay = GetOverlay();
    if (!overlay.Visible()) {
        touch.SetActive(false);
        if (before_open) {
            before_open();
        }
    }
    overlay.Toggle();
}

void InputBridge::ToggleTouch() {
    touch.SetActive(!touch.Active());
}

bool InputBridge::InGame() const {
    return render_window && render_window->isVisible();
}

void InputBridge::UpdateSelectBinding() {
    const auto& binding =
        Settings::values.current_input_profile.buttons[Settings::NativeButton::Select];
    if (binding == select_binding) {
        return;
    }
    select_binding = binding;
    select_button = Input::CreateDevice<Input::ButtonDevice>(select_binding);
    const Common::ParamPackage params(select_binding);
    select_key = params.Get("engine", "") == "keyboard" ? params.Get("code", -1) : -1;
    select_previous = true; // a held Select does not count until it is released
}

void InputBridge::CycleBeam(std::chrono::steady_clock::time_point time) {
    namespace P = Presentation;
    if (static_cast<P::Bottom>(P::bottom.load()) != P::Bottom::Hud) {
        return;
    }
    // The usual beam's slot is always there; the Ice and Grapple Beams once Samus has them.
    const int present = P::beam_slots.load() | 1;
    if (present == 1) {
        return;
    }
    // The bottom screen shows a tap a few frames later: right after one, go on from it.
    int current = time < beam_settled ? beam_target : P::beam_slot.load();
    if (current < 0) {
        current = 0;
    }
    int next = current;
    do {
        next = (next + 1) % static_cast<int>(BottomClassifier::BeamSlotRects.size());
    } while ((present & (1 << next)) == 0);
    const auto& [x0, y0, x1, y1] = BottomClassifier::BeamSlotRects[next];
    P::tap_x = (x0 + x1) / 2;
    P::tap_y = (y0 + y1) / 2;
    P::touch_tap_until = SteadyNanoseconds(time + std::chrono::milliseconds(100));
    beam_target = next;
    beam_settled = time + std::chrono::milliseconds(500);
}

void InputBridge::ToggleMorphBall(std::chrono::steady_clock::time_point time) {
    namespace P = Presentation;
    if (static_cast<P::Bottom>(P::bottom.load()) != P::Bottom::Hud) {
        return;
    }
    // The middle of the map (x 20-240, y 50-205); a tap there morphs, the next one unmorphs.
    P::tap_x = 130;
    P::tap_y = 127;
    P::touch_tap_until = SteadyNanoseconds(time + std::chrono::milliseconds(100));
}

bool InputBridge::SpareButton::Pressed() {
    const auto& current = Settings::values.current_input_profile.buttons[native];
    if (current != binding) {
        binding = current;
        device = Input::CreateDevice<Input::ButtonDevice>(binding);
        previous = true;
    }
    const bool held = device && device->GetStatus();
    const bool pressed = held && !previous;
    previous = held;
    return pressed;
}

bool InputBridge::OverRenderWindow(QObject* watched) const {
    auto* widget = qobject_cast<QWidget*>(watched);
    return widget && render_window && (widget == render_window || render_window->isAncestorOf(widget));
}

void InputBridge::Poll() {
    auto& overlay = GetOverlay();
    const auto time = std::chrono::steady_clock::now();
    const float seconds = std::min(.1f, std::chrono::duration<float>(time - last_poll).count());
    last_poll = time;

    std::array<bool, ButtonCount> now{};
    bool any = false;
    for (int i = 0; i < ButtonCount; ++i) {
        now[i] = buttons[i] && buttons[i]->GetStatus();
        any |= now[i];
    }
    any_held = any;
    const auto pressed = [&](int button) { return now[button] && !previous[button]; };
    float x = 0.f, y = 0.f;
    if (stick) {
        std::tie(x, y) = stick->GetStatus();
    }

    // Nothing here applies to the launcher or the loading screen.
    const bool in_game = InGame();

    // Select opens and closes the settings: the game treats it as Start, which it already has.
    UpdateSelectBinding();
    const bool select = select_button && select_button->GetStatus();
    if (select && !select_previous && !capture && in_game) {
        ToggleOverlay();
    }
    select_previous = select;

    // L3 toggles touch navigation while a menu is on screen (the pause menu's tools, a file's
    // copy and delete). In play the stick clicks do nothing here: pressed by accident while
    // moving or aiming, they used to open the pause menu or the touch screen.
    if (!capture && !overlay.Visible() && in_game && pressed(LeftStick) &&
        (touch.Active() || Presentation::panel_visible.load())) {
        ToggleTouch();
    }

    // The triggers (the 3DS ZR and ZL, unused by the game): the beams and the Morph Ball.
    const bool beam = zr.Pressed();
    const bool morph = zl.Pressed();
    if (!capture && !overlay.Visible() && !touch.Active() && in_game) {
        if (beam) {
            CycleBeam(time);
        }
        if (morph) {
            ToggleMorphBall(time);
        }
    }

    const bool visible = overlay.Visible();
    if (visible) {
        // While a control is being rebound, presses belong to the capture; releases still
        // reach the menu so it never believes a button is stuck down.
        for (int i = 0; i < ButtonCount; ++i) {
            if (now[i] != previous[i] && GamepadKeys[i] != ImGuiKey_None && (!capture || !now[i])) {
                overlay.Key(GamepadKeys[i], now[i]);
            }
        }
        if (!capture) {
            if (x != previous_x || y != previous_y) {
                overlay.Analog(ImGuiKey_GamepadLStickLeft, x < -.3f, std::max(0.f, -x));
                overlay.Analog(ImGuiKey_GamepadLStickRight, x > .3f, std::max(0.f, x));
                // The analog device reports up as positive.
                overlay.Analog(ImGuiKey_GamepadLStickUp, y > .3f, std::max(0.f, y));
                overlay.Analog(ImGuiKey_GamepadLStickDown, y < -.3f, std::max(0.f, -y));
            }
        }
        previous_x = x;
        previous_y = y;
    } else if (touch.Active()) {
        touch.Update();
        if (pressed(DpadUp)) {
            touch.Step(0, -1);
        }
        if (pressed(DpadDown)) {
            touch.Step(0, 1);
        }
        if (pressed(DpadLeft)) {
            touch.Step(-1, 0);
        }
        if (pressed(DpadRight)) {
            touch.Step(1, 0);
        }
        if (touch.Snapping()) {
            // The stick steps between buttons like the D-pad, repeating while held.
            const int sx = x > .5f ? 1 : x < -.5f ? -1 : 0;
            const int sy = y > .5f ? -1 : y < -.5f ? 1 : 0;
            if (sx != stick_direction_x || sy != stick_direction_y) {
                stick_direction_x = sx;
                stick_direction_y = sy;
                if (sx || sy) {
                    touch.Step(sx, sy);
                    stick_repeat = time + std::chrono::milliseconds(350);
                }
            } else if ((sx || sy) && time >= stick_repeat) {
                touch.Step(sx, sy);
                stick_repeat = time + std::chrono::milliseconds(140);
            }
        } else {
            touch.Glide(x, y, seconds);
        }
        // A is the finger: press to touch, hold and move to drag, release to lift.
        if (pressed(Choose)) {
            touch.Press();
        } else if (previous[Choose] && !now[Choose]) {
            touch.Release();
        }
        if (pressed(Back)) {
            touch.SetActive(false);
        }
    } else if (Presentation::block_game_input && !any) {
        // The menu or touch navigation closed: give the game its input back once the closing
        // button is released, so the B or Start that closed it never reaches the game.
        Presentation::block_game_input = false;
    }
    if (was_visible != visible && !visible) {
        // Release anything ImGui still believes is held.
        for (int i = 0; i < ButtonCount; ++i) {
            if (previous[i] && GamepadKeys[i] != ImGuiKey_None) {
                overlay.Key(GamepadKeys[i], false);
            }
        }
    }
    was_visible = visible;
    previous = now;
}

bool InputBridge::eventFilter(QObject* watched, QEvent* event) {
    auto& overlay = GetOverlay();
    switch (event->type()) {
    case QEvent::Show: {
        // Dialogs come from the emulator underneath; present them as the game's own.
        if (auto* box = qobject_cast<QMessageBox*>(watched)) {
            // Not inside paths (AppData\Roaming\Azahar\...), which must stay as they are.
            const auto rename = [](QString text) {
                static const QRegularExpression name(
                    QStringLiteral(R"((?<![\\/])Azahar(?![\\/]))"));
                return text.replace(name, QStringLiteral("Samus Returns"));
            };
            box->setWindowTitle(rename(box->windowTitle()));
            box->setText(rename(box->text()));
            box->setInformativeText(rename(box->informativeText()));
        } else if (auto* widget = qobject_cast<QWidget*>(watched);
                   widget && widget->isWindow() &&
                   widget->windowTitle().contains(QStringLiteral("Azahar"))) {
            widget->setWindowTitle(QStringLiteral("Metroid: Samus Returns"));
        }
        return false;
    }
    case QEvent::ShortcutOverride: {
        // While the menu or touch navigation is open, their keys must not trigger shortcuts.
        auto* key = static_cast<QKeyEvent*>(event);
        if (overlay.Visible() || touch.Active() || key->key() == Qt::Key_F1) {
            event->accept();
            return true;
        }
        return false;
    }
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool down = event->type() == QEvent::KeyPress;
        if (capture) {
            if (down && !key->isAutoRepeat()) {
                // Copy: the handler may clear the capture while it runs.
                const auto handler = capture;
                handler(key->key());
            } else if (!down && !key->isAutoRepeat()) {
                // Releases still reach the menu, or it would think the key that started the
                // capture is held and ignore its next press.
                if (const ImGuiKey mapped = MapKey(key->key()); mapped != ImGuiKey_None) {
                    overlay.Key(mapped, false);
                }
                return false; // and the game's keyboard, so no key stays held there
            }
            return true;
        }
        if (!InGame()) {
            return false;
        }
        if (key->key() == Qt::Key_F1) {
            if (down && !key->isAutoRepeat()) {
                ToggleOverlay();
            }
            return true;
        }
        if (!overlay.Visible()) {
            if (!OverRenderWindow(watched)) {
                return false;
            }
            if (key->key() == Qt::Key_Tab) {
                if (down && !key->isAutoRepeat()) {
                    ToggleTouch();
                }
                return true;
            }
            if (!touch.Active()) {
                return false;
            }
            // Touch navigation: arrows move, Enter or Space is the finger, Escape leaves.
            if (!down && !key->isAutoRepeat() &&
                (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter ||
                 key->key() == Qt::Key_Space)) {
                touch.Release();
            }
            if (down) {
                switch (key->key()) {
                case Qt::Key_Up:
                    touch.Step(0, -1);
                    break;
                case Qt::Key_Down:
                    touch.Step(0, 1);
                    break;
                case Qt::Key_Left:
                    touch.Step(-1, 0);
                    break;
                case Qt::Key_Right:
                    touch.Step(1, 0);
                    break;
                case Qt::Key_Return:
                case Qt::Key_Enter:
                case Qt::Key_Space:
                    if (!key->isAutoRepeat()) {
                        touch.Press();
                    }
                    break;
                case Qt::Key_Escape:
                    touch.SetActive(false);
                    break;
                default:
                    break;
                }
            }
            return true;
        }
        // A keyboard Select closes the menu (it opened it through the game's keyboard, which
        // the menu holds back while open).
        if (down && !key->isAutoRepeat() && key->key() == select_key && !capture) {
            ToggleOverlay();
            return true;
        }
        if (const ImGuiKey mapped = MapKey(key->key()); mapped != ImGuiKey_None && !key->isAutoRepeat()) {
            overlay.Key(mapped, down);
        } else if (mapped != ImGuiKey_None && down) {
            // Auto-repeat: a release and press let ImGui's own repeat logic restart.
            overlay.Key(mapped, true);
        }
        // Releases also reach the game's keyboard, so a key let go while the menu is open does
        // not stay held in the game (its input is blocked while the menu is open).
        return down;
    }
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick: {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!overlay.Visible() || !widget || !render_window ||
            widget->window() != render_window->window()) {
            return false;
        }
        // The event's own position, through the window it arrived in: depending on how the
        // native render surface is reached, the event goes to it or to the main window.
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint in_window = widget->mapTo(widget->window(), mouse->position().toPoint());
        const QPointF local = render_window->mapFrom(render_window->window(), in_window);
        if (!render_window->rect().contains(local.toPoint())) {
            return false;
        }
        const qreal ratio = render_window->devicePixelRatioF();
        overlay.MousePosition(static_cast<float>(local.x() * ratio),
                              static_cast<float>(local.y() * ratio));
        if (event->type() != QEvent::MouseMove) {
            const int button = mouse->button() == Qt::RightButton    ? 1
                               : mouse->button() == Qt::MiddleButton ? 2
                                                                      : 0;
            overlay.MouseButton(button, event->type() != QEvent::MouseButtonRelease);
        }
        return true;
    }
    case QEvent::Wheel: {
        if (!overlay.Visible() || !OverRenderWindow(watched)) {
            return false;
        }
        const auto* wheel = static_cast<QWheelEvent*>(event);
        overlay.MouseWheel(static_cast<float>(wheel->angleDelta().y()) / 120.f);
        return true;
    }
    default:
        return false;
    }
}

} // namespace MSR
