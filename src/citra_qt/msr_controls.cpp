// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QKeySequence>
#include "citra_qt/configuration/config.h"
#include "citra_qt/msr_controls.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "input_common/main.h"
#include "msr/controller_autoconfig.h"

namespace MSR::Controls {
namespace {

std::string KeyName(int code) {
    return QKeySequence(code).toString().toStdString();
}

void SaveProfile() {
    Settings::SaveProfile(Settings::values.current_input_profile_index);
}

} // namespace

const std::array<Row, 17> Rows{{
    {Settings::NativeButton::A, "A"},
    {Settings::NativeButton::B, "B"},
    {Settings::NativeButton::X, "X"},
    {Settings::NativeButton::Y, "Y"},
    {Settings::NativeButton::L, "L"},
    {Settings::NativeButton::R, "R"},
    {Settings::NativeButton::ZL, "ZL"},
    {Settings::NativeButton::ZR, "ZR"},
    {Settings::NativeButton::Start, "Start"},
    // The game treats Select as Start, so it opens these settings instead.
    {Settings::NativeButton::Select, "Select (settings)"},
    {Settings::NativeButton::Up, "D-pad up"},
    {Settings::NativeButton::Down, "D-pad down"},
    {Settings::NativeButton::Left, "D-pad left"},
    {Settings::NativeButton::Right, "D-pad right"},
    {Settings::NativeButton::Home, "Home"},
    {AnalogBase + Settings::NativeAnalog::CirclePad, "Circle Pad"},
    {AnalogBase + Settings::NativeAnalog::CStick, "C-Stick"},
}};

std::string BindingText(int id) {
    const auto& profile = Settings::values.current_input_profile;
    const bool analog = id >= AnalogBase;
    const Common::ParamPackage param(analog ? profile.analogs[id - AnalogBase]
                                            : profile.buttons[id]);
    if (!param.Has("engine")) {
        return "Not set";
    }
    const std::string engine = param.Get("engine", "");
    if (engine == "keyboard") {
        return "Key " + KeyName(param.Get("code", 0));
    }
    if (analog && engine == "analog_from_button") {
        std::string text = "Keys";
        for (const char* direction : {"up", "down", "left", "right"}) {
            text += " " + KeyName(Common::ParamPackage(param.Get(direction, "")).Get("code", 0));
        }
        return text;
    }
    if (analog && engine == "sdl") {
        const int axis = param.Get("axis_x", 0);
        return axis == 0   ? "Gamepad left stick"
               : axis == 2 ? "Gamepad right stick"
                           : "Gamepad axes " + param.Get("axis_x", "") + "/" + param.Get("axis_y", "");
    }
    const std::string text = InputCommon::ButtonToText(param);
    return engine == "sdl" ? "Gamepad " + text : text;
}

void Bind(int id, const std::string& param_text) {
    auto& profile = Settings::values.current_input_profile;
    // Gamepad bindings read every connected pad (MSR::ControllerAutoConfig::ReadEveryPad).
    Common::ParamPackage param(param_text);
    if (param.Get("engine", "") == "sdl") {
        param.Set("maptype", "all");
        profile.maptype = Settings::InputMappingType::AllControllers;
    }
    const std::string binding = param.Has("engine") ? param.Serialize() : param_text;
    if (id >= AnalogBase) {
        profile.analogs[id - AnalogBase] = binding;
    } else if (id >= 0) {
        profile.buttons[id] = binding;
    }
    SaveProfile();
}

bool ApplyGamepadLayout() {
    const auto controllers = InputCommon::GetConnectedGameControllers();
    if (controllers.empty()) {
        return false;
    }
    MSR::ControllerAutoConfig::Map(
        Settings::values.current_input_profile, controllers.front(),
        [](const Common::ParamPackage& pad, int button) {
            return InputCommon::GetControllerButtonBinds(pad, button);
        },
        [](const Common::ParamPackage& pad, int analog) {
            return InputCommon::GetControllerAnalogBinds(pad, analog);
        });
    SaveProfile();
    return true;
}

void ApplyKeyboardLayout() {
    auto& profile = Settings::values.current_input_profile;
    for (std::size_t i = 0; i < profile.buttons.size(); ++i) {
        profile.buttons[i] = InputCommon::GenerateKeyboardParam(QtConfig::default_buttons[i]);
    }
    for (std::size_t i = 0; i < profile.analogs.size(); ++i) {
        const auto& keys = QtConfig::default_analogs[i];
        profile.analogs[i] = InputCommon::GenerateAnalogParamFromKeys(keys[0], keys[1], keys[2],
                                                                      keys[3], keys[4], 0.5f);
    }
    SaveProfile();
}

bool ControllerConnected() {
    return !InputCommon::GetConnectedGameControllers().empty();
}

} // namespace MSR::Controls
