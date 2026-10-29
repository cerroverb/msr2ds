// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <string>

// The 3DS controls the player can rebind, shared by the launcher and the in-game menu. All of
// them edit the current input profile (Settings::values.current_input_profile).
namespace MSR::Controls {

/// Row ids at or above this are analog sticks (AnalogBase + Settings::NativeAnalog value);
/// below it they are Settings::NativeButton values.
inline constexpr int AnalogBase = 100;

struct Row {
    int id;
    const char* label;
};
extern const std::array<Row, 17> Rows;

/// What a row is bound to in the current profile, for display ("Key A", "Gamepad B").
std::string BindingText(int id);
/// Binds a row to an input (a serialized ParamPackage) and saves the profile.
void Bind(int id, const std::string& param);
/// Maps every control to the first connected gamepad by position and saves the profile; false
/// when no gamepad is connected.
bool ApplyGamepadLayout();
/// Restores the default keyboard bindings and saves the profile.
void ApplyKeyboardLayout();
bool ControllerConnected();

} // namespace MSR::Controls
