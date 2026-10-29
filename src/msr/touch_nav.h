// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>

namespace MSR {

/// Touch navigation (L3 or Tab): shows the touch screen and moves a highlight between its
/// buttons with the D-pad or left stick; A acts as a finger. A press taps; holding A and moving
/// drags (map pins, for instance). Known screens (the gameplay HUD and the pause menu's pages)
/// snap between their measured buttons; any other screen gets a free cursor. Qt thread; the
/// highlight and the touch are published through msr/presentation.h.
class TouchNavigator {
public:
    struct Target {
        int x0, y0, x1, y1; ///< Native bottom-screen pixels
    };

    bool Active() const {
        return active;
    }
    void SetActive(bool active);

    /// Steps the highlight (or the free or dragged point) in a direction: dx, dy in {-1, 0, 1}.
    void Step(int dx, int dy);
    /// Moves the free or dragged point with an analog stick (x right, y up).
    void Glide(float x, float y, float seconds);
    /// Puts the finger down at the highlight, and lifts it.
    void Press();
    void Release();
    /// Follows screen changes and publishes the highlight; call every poll.
    void Update();
    /// Whether the stick steps between buttons (true) or moves a point (free cursor, dragging).
    bool Snapping() const;

private:
    const std::vector<Target>* CurrentTargets() const;
    void Publish();

    bool active = false;
    bool holding = false;
    int focus = 0;
    float cursor_x = 160.f;
    float cursor_y = 120.f;
    const std::vector<Target>* targets = nullptr;
};

} // namespace MSR
