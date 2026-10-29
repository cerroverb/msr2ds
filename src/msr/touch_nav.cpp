// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include "msr/presentation.h"
#include "msr/touch_nav.h"

namespace MSR {
namespace {

using Target = TouchNavigator::Target;

// Buttons measured on the USA release, in native bottom-screen pixels.

// Gameplay HUD: the menu folder, the Aeion ability slots and the map.
const std::vector<Target> HudTargets = {
    {0, 212, 50, 240},     // pause menu folder
    {20, 50, 240, 205},    // map
    {253, 13, 317, 77},    // Aeion ability slots
    {253, 88, 317, 152},
    {253, 163, 317, 227},
};

// Pause menu, map page: page switches and the bottom toolbar.
const std::vector<Target> PauseMapTargets = {
    {2, 6, 24, 22},        // L: Samus page
    {296, 6, 318, 22},     // R: Samus page
    {2, 214, 48, 238},     // back
    {62, 216, 98, 238},    // map
    {102, 216, 138, 238},  // scan
    {141, 216, 180, 238},  // map pins
    {182, 216, 220, 238},  // information
    {223, 216, 262, 238},  // settings
    {270, 214, 318, 238},  // confirm
};

// Pause menu, map pins tool: back, the five pins and the bin. Hold A on a pin and move to drag
// it onto the map.
const std::vector<Target> MapPinsTargets = {
    {2, 214, 48, 238},     // back
    {63, 216, 87, 238},    // red pin
    {96, 216, 120, 238},   // green pin
    {129, 216, 153, 238},  // yellow pin
    {162, 216, 186, 238},  // light blue pin
    {195, 216, 219, 238},  // blue pin
    {282, 212, 318, 238},  // bin
};

// File select, choosing a file: the three files, back and confirm. Copy and delete stay unlit
// until a file is opened.
const std::vector<Target> FileListTargets = {
    {39, 33, 312, 86},     // file A
    {39, 93, 312, 146},    // file B
    {39, 153, 312, 206},   // file C
    {2, 220, 66, 240},     // back
    {254, 220, 318, 240},  // confirm
};

// File select, a file opened: its buttons, then the toolbar with copy and delete.
const std::vector<Target> FileOpenTargets = {
    {87, 89, 263, 121},    // Continue
    {87, 126, 263, 159},   // New Game
    {87, 164, 263, 197},   // Options
    {2, 220, 66, 240},     // back
    {96, 220, 124, 240},   // copy
    {196, 220, 224, 240},  // delete
    {254, 220, 318, 240},  // confirm
};

// Pause menu, Samus page: the two equipment grids either side of Samus, the description's
// scroll arrows, back and the page switch.
std::vector<Target> MakeSamusTargets() {
    std::vector<Target> targets = {
        {2, 6, 24, 22},        // L: map page
        {2, 214, 48, 238},     // back
        {306, 211, 319, 223},  // description up
        {306, 226, 319, 238},  // description down
    };
    const int left_rows[][2] = {{35, 62}, {65, 91}, {95, 122}, {125, 151}, {167, 193}};
    const int right_rows[][2] = {{35, 62}, {72, 98}, {107, 133}, {136, 163}, {167, 193}};
    const int left_columns[][2] = {{10, 52}, {57, 99}};
    const int right_columns[][2] = {{220, 264}, {268, 311}};
    for (const auto& column : left_columns) {
        for (const auto& row : left_rows) {
            targets.push_back({column[0], row[0], column[1], row[1]});
        }
    }
    for (const auto& column : right_columns) {
        for (const auto& row : right_rows) {
            targets.push_back({column[0], row[0], column[1], row[1]});
        }
    }
    return targets;
}
const std::vector<Target> PauseSamusTargets = MakeSamusTargets();

float CenterX(const Target& t) {
    return (t.x0 + t.x1) * .5f;
}
float CenterY(const Target& t) {
    return (t.y0 + t.y1) * .5f;
}

/// Index of the button focused when a screen appears.
int DefaultFocus(const std::vector<Target>* targets) {
    if (targets == &PauseMapTargets) {
        return 3; // the map tab
    }
    if (targets == &PauseSamusTargets) {
        return 4; // the first equipment slot
    }
    if (targets == &MapPinsTargets) {
        return 1; // the red pin
    }
    if (targets == &FileOpenTargets) {
        return 4; // copy, next to delete: the touch-only buttons
    }
    return 0;
}

long long SteadyNanoseconds(std::chrono::steady_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

} // namespace

const std::vector<Target>* TouchNavigator::CurrentTargets() const {
    namespace P = Presentation;
    const auto bottom = static_cast<P::Bottom>(P::bottom.load());
    if (bottom == P::Bottom::Hud) {
        return &HudTargets;
    }
    if (bottom == P::Bottom::Menu) {
        switch (static_cast<P::Page>(P::page.load())) {
        case P::Page::PauseMap:
            return &PauseMapTargets;
        case P::Page::PauseSamus:
            return &PauseSamusTargets;
        case P::Page::MapPins:
            return &MapPinsTargets;
        case P::Page::FileList:
            return &FileListTargets;
        case P::Page::FileOpen:
            return &FileOpenTargets;
        default:
            break;
        }
    }
    return nullptr;
}

bool TouchNavigator::Snapping() const {
    return targets != nullptr && !holding;
}

void TouchNavigator::SetActive(bool value) {
    if (holding) {
        Release();
    }
    active = value;
    Presentation::touch_mode = value;
    Presentation::force_panel = value;
    if (value) {
        Presentation::block_game_input = true;
        targets = CurrentTargets();
        focus = DefaultFocus(targets);
        cursor_x = 160.f;
        cursor_y = 120.f;
    }
    Publish();
}

void TouchNavigator::Update() {
    if (!active) {
        return;
    }
    // A tap often changes the screen (opening the pause menu, switching pages). Keep the
    // buttons of the screen the drag started on until the finger lifts.
    if (const auto* current = CurrentTargets(); current != targets && !holding) {
        targets = current;
        focus = DefaultFocus(targets);
    }
    Publish();
}

void TouchNavigator::Step(int dx, int dy) {
    if (!active) {
        return;
    }
    if (!Snapping()) {
        const float step = holding ? 6.f : 8.f;
        cursor_x = std::clamp(cursor_x + dx * step, 0.f, 319.f);
        cursor_y = std::clamp(cursor_y + dy * step, 0.f, 239.f);
        Publish();
        return;
    }
    // Nearest button in the pressed direction, favouring ones in line with the current one.
    const Target& from = (*targets)[focus];
    const float fx = CenterX(from), fy = CenterY(from);
    int best = -1;
    float best_score = std::numeric_limits<float>::max();
    for (int i = 0; i < static_cast<int>(targets->size()); ++i) {
        if (i == focus) {
            continue;
        }
        const Target& t = (*targets)[i];
        const float along = (CenterX(t) - fx) * dx + (CenterY(t) - fy) * dy;
        if (along <= 2.f) {
            continue;
        }
        const float across = std::abs((CenterX(t) - fx) * dy) + std::abs((CenterY(t) - fy) * dx);
        const float score = along + 2.5f * across;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best >= 0) {
        focus = best;
    }
    Publish();
}

void TouchNavigator::Glide(float x, float y, float seconds) {
    if (!active || Snapping()) {
        return;
    }
    constexpr float Speed = 220.f; // native pixels per second at full tilt
    cursor_x = std::clamp(cursor_x + x * Speed * seconds, 0.f, 319.f);
    cursor_y = std::clamp(cursor_y - y * Speed * seconds, 0.f, 239.f);
    Publish();
}

void TouchNavigator::Press() {
    if (!active || holding) {
        return;
    }
    if (targets) {
        // The drag starts from the focused button.
        const Target& t = (*targets)[focus];
        cursor_x = CenterX(t);
        cursor_y = CenterY(t);
    }
    holding = true;
    Publish();
    Presentation::touch_hold = true;
}

void TouchNavigator::Release() {
    if (!holding) {
        return;
    }
    holding = false;
    Presentation::touch_hold = false;
    // Keep the touch a little longer, so a quick press still lasts a few game frames.
    Presentation::touch_tap_until =
        SteadyNanoseconds(std::chrono::steady_clock::now() + std::chrono::milliseconds(80));
    if (targets) {
        // Back to snapping: focus the button nearest to where the finger lifted.
        float best = std::numeric_limits<float>::max();
        for (int i = 0; i < static_cast<int>(targets->size()); ++i) {
            const Target& t = (*targets)[i];
            const float d = std::hypot(CenterX(t) - cursor_x, CenterY(t) - cursor_y);
            if (d < best) {
                best = d;
                focus = i;
            }
        }
    }
    Publish();
}

void TouchNavigator::Publish() {
    namespace P = Presentation;
    if (active && Snapping()) {
        const Target& t = (*targets)[std::clamp(focus, 0, static_cast<int>(targets->size()) - 1)];
        P::touch_x0 = t.x0;
        P::touch_y0 = t.y0;
        P::touch_x1 = t.x1;
        P::touch_y1 = t.y1;
    } else {
        P::touch_x0 = P::touch_y0 = P::touch_x1 = P::touch_y1 = 0;
    }
    P::touch_x = static_cast<int>(cursor_x);
    P::touch_y = static_cast<int>(cursor_y);
    // The finger follows the point only while down; after it lifts, the brief release touch
    // stays where it was dropped.
    if (holding) {
        P::tap_x = static_cast<int>(cursor_x);
        P::tap_y = static_cast<int>(cursor_y);
    }
}

} // namespace MSR
