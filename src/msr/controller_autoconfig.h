// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Adapted from DekUI's MM3D::ControllerAutoConfig. A gamepad is mapped automatically while the
// active profile is still the untouched keyboard default or a profile this code generated, so
// custom bindings made in Settings are never overwritten.
//
// Buttons follow position, not label: 3DS A/B/X/Y become the right/bottom/top/left face buttons
// (Xbox B/A/Y/X). For Samus Returns that puts Shoot on the left face button and Jump on the
// bottom one, as on the 3DS. L/R are the bumpers, ZL/ZR the triggers.
//
// The bindings read every connected gamepad (maptype "all"), as single-player PC games do: under
// Steam Input each controller reaches the game as one of Steam's virtual pads, with its own GUID
// and slot (a Steam Controller has no other way in), and a second pad would otherwise do nothing.
// The GUID the profile was made from is kept, to tell which pad it follows.

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>
#include "common/param_package.h"
#include "common/settings.h"

namespace MSR::ControllerAutoConfig {

inline constexpr char ProfileName[] = "Controller (automatic)";

using Buttons = std::array<std::string, Settings::NativeButton::NumButtons>;
using Analogs = std::array<std::string, Settings::NativeAnalog::NumAnalogs>;

/// The same bindings. `loosely` ignores which pads they read (profiles made before every pad was
/// read) and whether a binding was captured from a press ("down", set by the Controls tab).
template <class Bindings>
bool SameBindings(const Bindings& a, const Bindings& b, bool loosely = false) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) {
            continue;
        }
        Common::ParamPackage first(a[i]), second(b[i]);
        if (loosely) {
            for (auto* param : {&first, &second}) {
                param->Erase("maptype");
                param->Erase("down");
            }
        }
        if (!first.Has("engine") || !second.Has("engine") ||
            first.Serialize() != second.Serialize()) {
            return false;
        }
    }
    return true;
}

/// The profile still holds the default keyboard bindings (motion and touch settings ignored).
inline bool Untouched(const Settings::InputProfile& profile, const Buttons& keyboard_buttons,
                      const Analogs& keyboard_analogs) {
    return profile.maptype == Settings::InputMappingType::GuidPort &&
           SameBindings(profile.buttons, keyboard_buttons) &&
           SameBindings(profile.analogs, keyboard_analogs);
}

/// Builds the mapping in a copy and publishes it only when every game control has a binding.
/// Controls a standard pad lacks (Home, Debug, GPIO14) keep their keyboard bindings.
template <class ButtonBinds, class AnalogBinds>
bool Map(Settings::InputProfile& profile, const Common::ParamPackage& controller,
         ButtonBinds button_binds, AnalogBinds analog_binds) {
    if (controller.Get("engine", "") != "sdl" || controller.Get("api", "") != "controller" ||
        controller.Get("guid", "").empty()) {
        return false;
    }
    auto result = profile;
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        auto binding = button_binds(controller, i);
        if (binding.Has("engine")) {
            binding.Set("maptype", "all");
            result.buttons[i] = binding.Serialize();
        } else if (i <= Settings::NativeButton::Select || i == Settings::NativeButton::ZL ||
                   i == Settings::NativeButton::ZR) {
            return false;
        }
    }
    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        auto binding = analog_binds(controller, i);
        if (!binding.Has("engine")) {
            return false;
        }
        binding.Set("maptype", "all");
        result.analogs[i] = binding.Serialize();
    }
    result.name = ProfileName;
    result.maptype = Settings::InputMappingType::AllControllers;
    profile = std::move(result);
    return true;
}

/// Makes a profile's gamepad bindings read every connected pad. Profiles saved before read one pad
/// by its GUID, which Steam Input hides behind its virtual pads. Returns whether anything changed.
inline bool ReadEveryPad(Settings::InputProfile& profile) {
    bool changed = false;
    const auto convert = [&changed](std::string& binding) {
        Common::ParamPackage param(binding);
        if (param.Get("engine", "") != "sdl" || param.Get("maptype", "") == "all") {
            return;
        }
        param.Set("maptype", "all");
        binding = param.Serialize();
        changed = true;
    };
    std::for_each(profile.buttons.begin(), profile.buttons.end(), convert);
    std::for_each(profile.analogs.begin(), profile.analogs.end(), convert);
    if (changed) {
        profile.maptype = Settings::InputMappingType::AllControllers;
    }
    return changed;
}

inline bool SameDevice(const Common::ParamPackage& a, const Common::ParamPackage& b) {
    return a.Get("guid", "") == b.Get("guid", "") && a.Get("port", 0) == b.Get("port", 0);
}

/// The profile is one this code generated and the user has not edited since.
template <class ButtonBinds, class AnalogBinds>
bool Managed(const Settings::InputProfile& profile, ButtonBinds button_binds,
             AnalogBinds analog_binds) {
    if (profile.name != ProfileName ||
        (profile.maptype != Settings::InputMappingType::GuidPort &&
         profile.maptype != Settings::InputMappingType::AllControllers)) {
        return false;
    }
    Common::ParamPackage device(profile.buttons[Settings::NativeButton::A]);
    // Map() only reads engine/api/guid/port; drop the bound button so the layout is re-derived
    // the same way it was generated.
    device.Erase("button");
    auto expected = profile;
    return Map(expected, device, button_binds, analog_binds) &&
           SameBindings(profile.buttons, expected.buttons, true) &&
           SameBindings(profile.analogs, expected.analogs, true);
}

/// Returns the profile to switch to, or nothing when the current mapping should stay.
template <class ButtonBinds, class AnalogBinds>
std::optional<Settings::InputProfile> Select(const Settings::InputProfile& current,
                                             std::vector<Common::ParamPackage> controllers,
                                             const Buttons& keyboard_buttons,
                                             const Analogs& keyboard_analogs,
                                             ButtonBinds button_binds, AnalogBinds analog_binds) {
    const bool managed = Managed(current, button_binds, analog_binds);
    if (!managed && !Untouched(current, keyboard_buttons, keyboard_analogs)) {
        return {};
    }
    // Prefer the pad that is already bound so a second pad does not steal the mapping.
    const Common::ParamPackage bound(current.buttons[Settings::NativeButton::A]);
    std::stable_partition(controllers.begin(), controllers.end(),
                          [&](const auto& pad) { return managed && SameDevice(pad, bound); });
    for (const auto& controller : controllers) {
        auto result = current;
        if (!Map(result, controller, button_binds, analog_binds)) {
            continue;
        }
        if (SameBindings(result.buttons, current.buttons) &&
            SameBindings(result.analogs, current.analogs)) {
            return {};
        }
        return result;
    }
    return {};
}

} // namespace MSR::ControllerAutoConfig
