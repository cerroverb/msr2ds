// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// State shared by the frontend, core (layout, touch, HID) and the Vulkan renderer for the
// single-screen Samus Returns presentation. Header-only and inert until the frontend sets
// `enabled`, so builds without the game folder behave exactly like upstream.

#pragma once

#include <atomic>

namespace MSR::Presentation {

/// Single 16:9 screen: the top screen fills the window and the bottom screen's HUD elements
/// (energy, missiles, Aeion, map, DNA and Metroid counts) are composited onto it.
inline std::atomic<bool> enabled{false};

/// Widen the top screen to 16:9: depth-tested world draws are squeezed by NativeAspect /
/// WideAspect before clipping, revealing more of the level, and presentation stretches it back.
inline std::atomic<bool> widescreen{true};

inline constexpr float NativeAspect = 400.f / 240.f;
inline constexpr float WideAspect = 16.f / 9.f;
inline constexpr float WideScale = NativeAspect / WideAspect; // 15/16

/// The player's camera zoom: above 1 closer, below 1 wider. It scales the top screen's world draws
/// (the depth-tested draws the widescreen squeeze takes; the sky and other backdrops, drawn
/// without depth test, keep filling the screen) and leaves 2D layers and the HUD as they are.
inline std::atomic<float> zoom{1.f};
/// The scale in effect, set by the renderer each frame: the zoom in play, eased in and out, and
/// 1 on the title screens (renderer_vulkan.cpp DrawMsrScreens).
inline std::atomic<float> world_zoom{1.f};

/// HUD element size relative to the game's own pixels (1 = same size as on the 3DS).
inline std::atomic<float> hud_scale{.6f};
inline std::atomic<float> hud_opacity{1.f};
inline std::atomic<float> minimap_opacity{0.85f};
inline std::atomic<float> minimap_size{1.15f};
inline std::atomic<bool> show_minimap{true};
inline std::atomic<bool> show_counters{true};
inline std::atomic<bool> show_abilities{true};

/// Height of the bottom-screen panel (pause menu, file select...) relative to the game view.
inline std::atomic<float> panel_scale{1.f};

/// Opacity of the dark card behind the title menus (file select, gallery...), which are drawn
/// without their backdrop over the game view.
inline std::atomic<float> menu_backdrop{.75f};

/// What the bottom screen is showing, classified from its pixels by the renderer.
enum class Bottom : int {
    Unknown, ///< Nothing classified yet: hidden.
    Hud,     ///< Gameplay HUD: composite its elements onto the game view.
    Logo,    ///< Idle emblem shown during cutscenes and loading: hidden.
    Menu,    ///< Framed menus (pause, file select): the whole screen as a centered panel.
    Prompt,  ///< Text on the bare backdrop (title prompt, prologue): floating, like subtitles.
};
inline std::atomic<int> bottom{static_cast<int>(Bottom::Unknown)};

/// Which framed menu is showing, for touch navigation targets.
enum class Page : int {
    Other,
    PauseMap,   ///< Pause menu, map page (toolbar at the bottom)
    PauseSamus, ///< Pause menu, Samus page (equipment grid)
    MapPins,    ///< Pause menu, map pins tool (pins along the bottom)
    FileList,   ///< File select, choosing a file
    FileOpen,   ///< File select, a file opened (Continue, New Game, Options; copy and delete)
    Gallery,    ///< Gallery viewer: the art on the top screen, only its counter on the bottom
};
inline std::atomic<int> page{static_cast<int>(Page::Other)};

/// The gameplay HUD's beam slots (bottom screen, right): 0 the Aeion ability, i.e. the usual
/// beam; 1 the Ice Beam; 2 the Grapple Beam. One is lit at a time; tapping another lights it.
/// The lit one (-1: not known) and the ones holding an item (bits), as the HUD last showed
/// them with the row lit: in Morph Ball the game greys it out and they are kept.
inline std::atomic<int> beam_slot{-1};
inline std::atomic<int> beam_slots{0};

/// Touch navigation (Tab, or L3 in the menus): the touch screen is shown and a highlight, moved with the D-pad
/// or left stick, marks where A taps. A rectangle (native bottom-screen pixels) when snapping
/// between known buttons, otherwise a free cursor at touch_x/touch_y.
inline std::atomic<bool> touch_mode{false};
inline std::atomic<int> touch_x{160};
inline std::atomic<int> touch_y{120};
inline std::atomic<int> touch_x0{0}, touch_y0{0}, touch_x1{0}, touch_y1{0};

/// A tap injected into the touch screen until this steady-clock time (ns), at tap_x/tap_y.
inline std::atomic<long long> touch_tap_until{0};
/// The touch screen is held at tap_x/tap_y (touch navigation, A held: drag).
inline std::atomic<bool> touch_hold{false};
inline std::atomic<int> tap_x{0};
inline std::atomic<int> tap_y{0};

/// Where the bottom-screen panel is drawn, in framebuffer pixels (for the overlay's hints).
inline std::atomic<int> panel_x0{0}, panel_y0{0}, panel_x1{0}, panel_y1{0};

/// Emulated time (us) when the game last saw Start pressed (HID). From gameplay it opens the
/// pause menu, whose first frames are its text over a still-black screen: the renderer must not
/// take them for a prompt.
inline std::atomic<long long> start_pressed_us{-1};

/// The title menus' bottom-screen backdrop was left out of the last bottom frame
/// (vk_rasterizer.cpp): the panel is composited as drawn over black, letting the game show
/// through.
inline std::atomic<bool> backdrop_removed{false};

/// The player asked to see the whole bottom screen (to touch the map, for instance).
inline std::atomic<bool> force_panel{false};

/// The bottom-screen panel is on screen, so touches should reach the game.
inline std::atomic<bool> panel_visible{false};

/// The settings overlay is open: the game keeps running but receives no input.
inline std::atomic<bool> block_game_input{false};

} // namespace MSR::Presentation
