// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The game's face button icons (A, B, X, Y) as an Xbox pad labels the buttons they are bound to.
// The game draws them from three textures: its font sheet (the icons inside text, "Press Ⓐ to
// Start"), its interface atlas (the same icons as sprites) and the top screen's atlas (only A and
// B, the dialogs' "Ⓐ ✔" and "Ⓑ ✖"). With the usual bindings the 3DS A
// is the right face button, which Xbox labels B, so the A icon shows the game's own B icon, and
// so on: the renderer copies the icon cells within each texture as it uploads it
// (rasterizer_cache.h), and the in-game menu names the buttons the same way. Other pads keep the
// game's icons. Header-only and inert while `shown` is the identity, like msr/texture_packs.h.
// The native runtime (native/runtime/msr/glyphs.cpp) also sets `device` from the last input: a
// PlayStation pad gets its symbols drawn in the cells (`symbols`), and the control reminders name
// that pad's buttons, or the keyboard's keys (Keys).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"

namespace MSR::ButtonGlyphs {

/// The game's face buttons, in Settings::NativeButton order, which SDL_GameControllerButton
/// shares for an Xbox pad's A, B, X and Y.
enum Face : u32 { A, B, X, Y, FaceCount };

/// Two bits per face: the face whose icon it shows. The identity shows each its own.
inline constexpr u32 Identity = 0 | 1 << 2 | 2 << 4 | 3 << 6;
/// The positional bindings on an Xbox pad: A on its B, B on its A, X on its Y, Y on its X.
inline constexpr u32 XboxPositional = 1 | 0 << 2 | 3 << 4 | 2 << 6;

/// Set by the frontend from the connected pad and the bindings.
inline std::atomic<u32> shown{Identity};

inline constexpr u32 Shown(u32 mapping, u32 face) {
    return (mapping >> (face * 2)) & 3;
}

/// A face icon's cell, in the texture as dumped (top row first). All are 13 x 13. NoCell: the
/// sheet has no icon for that face (the top screen's atlas has only A and B).
struct Cell {
    u32 x, y;
};
inline constexpr Cell NoCell{~0u, ~0u};
inline constexpr bool Has(const Cell& cell) {
    return cell.x != NoCell.x;
}
inline constexpr u32 CellSize = 13;
/// The L and R icons (a tab with the letter cut in, one top corner cut off) are 18 x 13.
inline constexpr u32 ShoulderWidth = 18, ShoulderHeight = 13;
enum Shoulder : u32 { ShoulderL, ShoulderR, ShoulderCount };

struct Sheet {
    u64 hash; ///< CityHash64 of the texture's data in emulated memory (Azahar's "new" hash)
    u32 width, height;
    u32 format; ///< VideoCore::PixelFormat
    std::array<Cell, FaceCount> cells;
    std::array<Cell, ShoulderCount> shoulders; ///< L, R
};

/// Samus Returns (USA).
inline constexpr std::array<Sheet, 3> Sheets{{
    // The font sheet (IA8), several sizes of text and the button icons.
    {0x5223C28BC4470EB0, 512, 256, 5, {{{23, 204}, {87, 143}, {25, 221}, {105, 139}}}, {{{2, 226}, {63, 159}}}},
    // The interface atlas (I8): the same icons beside the D-pad, L, R and START sprites.
    {0xB0F410A02C69F9AB, 1024, 512, 7, {{{706, 66}, {691, 66}, {706, 51}, {691, 51}}}, {{{720, 66}, {720, 51}}}},
    // The top screen's interface atlas (RGBA8): the dialogs' "(A) check mark" and "(B) cross"
    // ("Save your progress?"), only A and B.
    {0x6EFC9BD1FD290C7A, 512, 256, 0, {{{415, 46}, {426, 31}, NoCell, NoCell}}, {{NoCell, NoCell}}},
}};

/// A sheet's pixels: the game's texture as the renderer decodes it, or a texture pack's
/// replacement of it, which may be larger by a whole factor. Rows bottom first, as decoded.
struct Image {
    u8* pixels;
    u32 width, height;
    u32 bpp; ///< Bytes per pixel
};

/// The four face icons' pixels, in Face order.
using Icons = std::array<std::vector<u8>, FaceCount>;

/// The player's input device, by the one used last. The emulator-based build leaves it Unknown
/// (and sets `shown` itself); the native runtime sets all of these.
enum Device : u32 { Unknown, Keyboard, Xbox, PlayStation, Nintendo };
inline std::atomic<u32> device{Unknown};

/// A PlayStation symbol drawn in a face's cell instead of an icon: 3 bits per face, 0 = none.
enum Symbol : u32 { NoSymbol, Cross, Circle, Square, Triangle };
inline std::atomic<u32> symbols{0};
inline constexpr u32 SymbolOf(u32 all, u32 face) {
    return (all >> (face * 3)) & 7;
}

/// A key's letter or digit drawn the same way (the keyboard: the key each face is bound to), 8
/// bits per face (the character, 0 = none).
inline std::atomic<u32> letters{0};
inline constexpr char LetterOf(u32 all, u32 face) {
    return static_cast<char>((all >> (face * 8)) & 0xFF);
}
/// The same for the L and R icons (the keyboard: the keys L and R are bound to), 8 bits each.
inline std::atomic<u32> shoulder_letters{0};

/// A plain stroke font for those: polylines on a 4 x 6 grid (x right, y down), -1 ends a line.
inline const std::vector<float>& LetterStrokes(char c) {
    static const std::array<std::vector<float>, 36> font{{
        {0, 6, 0, 2, 2, 0, 4, 2, 4, 6, -1, 0, 4, 4, 4},                                                        // A
        {0, 0, 0, 6, 3, 6, 4, 5, 4, 4, 3, 3, 0, 3, -1, 3, 3, 4, 2, 4, 1, 3, 0, 0, 0},                          // B
        {4, 1, 3, 0, 1, 0, 0, 1, 0, 5, 1, 6, 3, 6, 4, 5},                                                      // C
        {0, 0, 0, 6, 2, 6, 4, 4, 4, 2, 2, 0, 0, 0},                                                            // D
        {4, 0, 0, 0, 0, 6, 4, 6, -1, 0, 3, 3, 3},                                                              // E
        {4, 0, 0, 0, 0, 6, -1, 0, 3, 3, 3},                                                                    // F
        {4, 1, 3, 0, 1, 0, 0, 1, 0, 5, 1, 6, 3, 6, 4, 5, 4, 3, 2, 3},                                          // G
        {0, 0, 0, 6, -1, 4, 0, 4, 6, -1, 0, 3, 4, 3},                                                          // H
        {1, 0, 3, 0, -1, 2, 0, 2, 6, -1, 1, 6, 3, 6},                                                          // I
        {4, 0, 4, 5, 3, 6, 1, 6, 0, 5},                                                                        // J
        {0, 0, 0, 6, -1, 4, 0, 0, 4, -1, 1.4f, 2.6f, 4, 6},                                                    // K
        {0, 0, 0, 6, 4, 6},                                                                                    // L
        {0, 6, 0, 0, 2, 3, 4, 0, 4, 6},                                                                        // M
        {0, 6, 0, 0, 4, 6, 4, 0},                                                                              // N
        {1, 0, 3, 0, 4, 1, 4, 5, 3, 6, 1, 6, 0, 5, 0, 1, 1, 0},                                                // O
        {0, 6, 0, 0, 3, 0, 4, 1, 4, 2, 3, 3, 0, 3},                                                            // P
        {1, 0, 3, 0, 4, 1, 4, 5, 3, 6, 1, 6, 0, 5, 0, 1, 1, 0, -1, 2.5f, 4.5f, 4, 6},                          // Q
        {0, 6, 0, 0, 3, 0, 4, 1, 4, 2, 3, 3, 0, 3, -1, 2, 3, 4, 6},                                            // R
        {4, 1, 3, 0, 1, 0, 0, 1, 0, 2, 1, 3, 3, 3, 4, 4, 4, 5, 3, 6, 1, 6, 0, 5},                              // S
        {0, 0, 4, 0, -1, 2, 0, 2, 6},                                                                          // T
        {0, 0, 0, 5, 1, 6, 3, 6, 4, 5, 4, 0},                                                                  // U
        {0, 0, 2, 6, 4, 0},                                                                                    // V
        {0, 0, 1, 6, 2, 3, 3, 6, 4, 0},                                                                        // W
        {0, 0, 4, 6, -1, 4, 0, 0, 6},                                                                          // X
        {0, 0, 2, 3, 4, 0, -1, 2, 3, 2, 6},                                                                    // Y
        {0, 0, 4, 0, 0, 6, 4, 6},                                                                              // Z
        {1, 0, 3, 0, 4, 1, 4, 5, 3, 6, 1, 6, 0, 5, 0, 1, 1, 0, -1, 3.5f, 1, .5f, 5},                           // 0
        {1, 1, 2, 0, 2, 6, -1, 1, 6, 3, 6},                                                                    // 1
        {0, 1, 1, 0, 3, 0, 4, 1, 4, 2, 0, 6, 4, 6},                                                            // 2
        {0, 1, 1, 0, 3, 0, 4, 1, 4, 2, 3, 3, 4, 4, 4, 5, 3, 6, 1, 6, 0, 5, -1, 1.5f, 3, 3, 3},                 // 3
        {3, 6, 3, 0, 0, 4, 4, 4},                                                                              // 4
        {4, 0, 0, 0, 0, 3, 3, 3, 4, 4, 4, 5, 3, 6, 1, 6, 0, 5},                                                // 5
        {3, 0, 1, 0, 0, 1, 0, 5, 1, 6, 3, 6, 4, 5, 4, 4, 3, 3, 0, 3},                                          // 6
        {0, 0, 4, 0, 1.5f, 6},                                                                                 // 7
        {1, 3, 0, 2, 0, 1, 1, 0, 3, 0, 4, 1, 4, 2, 3, 3, 1, 3, 0, 4, 0, 5, 1, 6, 3, 6, 4, 5, 4, 4, 3, 3},      // 8
        {4, 3, 1, 3, 0, 2, 0, 1, 1, 0, 3, 0, 4, 1, 4, 5, 3, 6, 1, 6},                                          // 9
    }};
    static const std::vector<float> none, space{0, 3.2f, 0, 4.8f, 4, 4.8f, 4, 3.2f};   // the space bar's sign
    if (c == ' ') return space;
    if (c >= 'A' && c <= 'Z') return font[static_cast<std::size_t>(c - 'A')];
    if (c >= '0' && c <= '9') return font[26 + static_cast<std::size_t>(c - '0')];
    return none;
}

/// The control reminders' names (native runtime): the key bound to a game button and the pad
/// button bound to it, by control (A, B, X, Y, L, R, ZL, ZR, Start, Select), and a pad's own
/// buttons by role (`PadRole`). nullptr: the 3DS names.
enum Control : u32 { CA, CB, CX, CY, CL, CR, CZL, CZR, CStart, CSelect };
/// PadChoose and PadCancel: the buttons that choose and go back in menus (msr-native: the bottom and right ones, the
/// right and bottom ones on a Nintendo pad).
enum PadRole : u32 { PadSouth, PadEast, PadWest, PadNorth, PadL1, PadR1, PadL3, PadBack, PadStick, PadDpad, PadChoose, PadCancel };
/// The keyboard's own roles, asked of key_name past the controls (msr-native: from its bindings): the circle pad's
/// keys, moving in touch mode, touch mode's key, its finger and its exit.
enum KeyRole : u32 { KeyStick = 100, KeyMove, KeyTouch, KeyTap, KeyLeave };
using NameFn = const char* (*)(u32);
inline std::atomic<NameFn> key_name{nullptr};
inline std::atomic<NameFn> pad_name{nullptr};
inline std::atomic<NameFn> role_name{nullptr};

/// Calls `copy(row in the image, row in the icon, bytes)` for each row of each face's cell.
template <class Copy>
void ForEachRow(const Sheet& sheet, const Image& image, Copy&& copy) {
    const u32 scale = image.width / sheet.width;
    const u32 size = CellSize * scale;
    const std::size_t row_bytes = std::size_t{size} * image.bpp;
    for (u32 face = 0; face < FaceCount; ++face) {
        const Cell& cell = sheet.cells[face];
        if (!Has(cell)) {
            continue;
        }
        for (u32 y = 0; y < size; ++y) {
            const std::size_t row = image.height - 1 - (cell.y * scale + y);
            copy(face, image.pixels + (row * image.width + cell.x * scale) * image.bpp,
                 y * row_bytes, row_bytes);
        }
    }
}

/// Whether `image` is the sheet or a replacement of it the icons can be found in.
inline bool Fits(const Sheet& sheet, const Image& image) {
    return image.width % sheet.width == 0 && image.width / sheet.width != 0 &&
           image.height * sheet.width == sheet.height * image.width;
}

/// The game's icons, read before any are rewritten.
inline Icons Read(const Sheet& sheet, const Image& image) {
    Icons icons;
    const std::size_t size = std::size_t{CellSize} * (image.width / sheet.width);
    for (auto& icon : icons) {
        icon.resize(size * size * image.bpp);
    }
    ForEachRow(sheet, image, [&](u32 face, const u8* row, std::size_t offset, std::size_t bytes) {
        std::memcpy(icons[face].data() + offset, row, bytes);
    });
    return icons;
}

/// Draws in each face's cell the icon `mapping` shows for it (its own where the sheet lacks that one).
inline void Write(const Sheet& sheet, const Image& image, const Icons& icons, u32 mapping) {
    ForEachRow(sheet, image, [&](u32 face, u8* row, std::size_t offset, std::size_t bytes) {
        const u32 shown_face = Shown(mapping, face);
        std::memcpy(row, icons[Has(sheet.cells[shown_face]) ? shown_face : face].data() + offset, bytes);
    });
}

/// A PlayStation symbol (or a key's `letter`) in an icon's cell (`cw` x `ch`: a face's disc, or the
/// L and R icons' tab): the icon's own shape with its letter filled in and the symbol cut into it as
/// the game cuts its letters (`ink`: the letter's colour, transparent in the font sheet, dark in
/// the interface atlas). Any scale; RGBA. The cell is written whole, or not at all.
inline void DrawMark(u8* cell, u32 cw, u32 ch, std::size_t row_bytes, const u8* icon, u32 symbol, char letter) {
    const std::vector<float>& strokes = LetterStrokes(letter);
    if ((symbol == NoSymbol && strokes.empty()) || cw < 8 || ch < 8) {
        return;
    }
    const u32 size = std::min(cw, ch);
    const auto at = [&](const u8* base, std::size_t stride, u32 x, u32 y) { return base + y * stride + x * 4; };
    const std::size_t icon_row = std::size_t{cw} * 4;
    const auto value = [&](const u8* p) { return std::min(p[0], p[3]); };
    // the disc's colour (brightest) and the letter's (darkest inside the disc)
    u8 disc[4] = {255, 255, 255, 255}, ink[4] = {0, 0, 0, 0};
    int best = -1, worst = 256;
    for (u32 y = ch / 4; y < ch - ch / 4; ++y) {
        for (u32 x = cw / 4; x < cw - cw / 4; ++x) {
            const u8* p = at(icon, icon_row, x, y);
            if (value(p) > best) { best = value(p); std::memcpy(disc, p, 4); }
            if (value(p) < worst) { worst = value(p); std::memcpy(ink, p, 4); }
        }
    }
    if (best - worst < 64) {
        return;   // not a disc with a letter
    }
    const float s = static_cast<float>(size), cx = cw * .5f, cy = ch * .5f, w = s * .068f;
    const auto segment = [](float px, float py, float ax, float ay, float bx, float by) {
        const float dx = bx - ax, dy = by - ay;
        const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
        return std::hypot(px - ax - t * dx, py - ay - t * dy);
    };
    // a letter: 4 x 6 grid units in a box half the cell high, a little thinner strokes
    const float unit = s * .5f / 6.f, lx = cx - 2.f * unit, ly = cy - 3.f * unit;
    const float width = strokes.empty() ? w : s * .062f;
    const auto distance = [&](float px, float py) {
        if (!strokes.empty()) {
            float best_d = 1e9f;
            for (std::size_t i = 0; i + 3 < strokes.size(); i += 2) {
                if (strokes[i] < 0.f) { i -= 1; continue; }   // a line ends: the next starts after the -1
                if (strokes[i + 2] < 0.f) { i += 1; continue; }
                best_d = std::min(best_d, segment(px, py, lx + strokes[i] * unit, ly + strokes[i + 1] * unit,
                                                  lx + strokes[i + 2] * unit, ly + strokes[i + 3] * unit));
            }
            return best_d;
        }
        switch (symbol) {
        case Cross: {
            const float k = s * .2f;
            return std::min(segment(px, py, cx - k, cy - k, cx + k, cy + k), segment(px, py, cx - k, cy + k, cx + k, cy - k));
        }
        case Circle:
            return std::fabs(std::hypot(px - cx, py - cy) - s * .225f);
        case Square: {
            const float k = s * .2f;
            return std::min({segment(px, py, cx - k, cy - k, cx + k, cy - k), segment(px, py, cx + k, cy - k, cx + k, cy + k),
                             segment(px, py, cx + k, cy + k, cx - k, cy + k), segment(px, py, cx - k, cy + k, cx - k, cy - k)});
        }
        default: {   // Triangle, point up (rows run down the cell), its centre a little low
            const float r = s * .27f, ty = cy + s * .03f;
            const float ax = cx, ay = ty - r, bx = cx + r * .866f, by = ty + r * .5f, dx = cx - r * .866f, dy = ty + r * .5f;
            return std::min({segment(px, py, ax, ay, bx, by), segment(px, py, bx, by, dx, dy), segment(px, py, dx, dy, ax, ay)});
        }
        }
    };
    for (u32 y = 0; y < ch; ++y) {
        // the disc: the row from its first to its last bright pixel, the letter filled in
        int first = -1, last = -1;
        for (u32 x = 0; x < cw; ++x) {
            if (value(at(icon, icon_row, x, y)) * 2 >= best) { if (first < 0) first = static_cast<int>(x); last = static_cast<int>(x); }
        }
        for (u32 x = 0; x < cw; ++x) {
            u8* out = cell + y * row_bytes + x * 4;
            const u8* in = at(icon, icon_row, x, y);
            const bool inside = first >= 0 && static_cast<int>(x) > first && static_cast<int>(x) < last;
            u8 base[4];
            std::memcpy(base, inside ? disc : in, 4);
            float cover = 0.f;   // 4 x 4 samples
            for (int j = 0; j < 4; ++j) {
                for (int i = 0; i < 4; ++i) {
                    cover += distance(static_cast<float>(x) + (i + .5f) / 4.f, static_cast<float>(y) + (j + .5f) / 4.f) < width ? 1.f / 16 : 0.f;
                }
            }
            if (!inside) {
                cover = 0.f;
            }
            for (int k = 0; k < 4; ++k) {
                out[k] = static_cast<u8>(std::lround(base[k] + (ink[k] - base[k]) * cover));
            }
        }
    }
}

inline void DrawSymbol(u8* cell, u32 size, std::size_t row_bytes, const u8* icon, u32 symbol, char letter = 0) {
    DrawMark(cell, size, size, row_bytes, icon, symbol, letter);
}

/// The key letters `keys` names (ShoulderL, ShoulderR: 8 bits each) cut into the L and R icons
/// in place of their letters.
inline void WriteShoulders(const Sheet& sheet, const Image& image, u32 keys) {
    if (!keys || image.bpp != 4) {
        return;
    }
    const u32 scale = image.width / sheet.width, w = ShoulderWidth * scale, h = ShoulderHeight * scale;
    for (u32 i = 0; i < ShoulderCount; ++i) {
        const char letter = LetterOf(keys, i);
        if (LetterStrokes(letter).empty() || !Has(sheet.shoulders[i])) {
            continue;
        }
        // the cell top row first (the image's rows run bottom first)
        const Cell& cell = sheet.shoulders[i];
        const auto row_of = [&](u32 y) {
            return image.pixels + ((image.height - 1 - (cell.y * scale + y)) * std::size_t{image.width} + cell.x * scale) * 4;
        };
        std::vector<u8> icon(std::size_t{w} * h * 4);
        for (u32 y = 0; y < h; ++y) {
            std::memcpy(icon.data() + std::size_t{y} * w * 4, row_of(y), std::size_t{w} * 4);
        }
        std::vector<u8> out = icon;
        DrawMark(out.data(), w, h, std::size_t{w} * 4, icon.data(), NoSymbol, letter);
        for (u32 y = 0; y < h; ++y) {
            std::memcpy(row_of(y), out.data() + std::size_t{y} * w * 4, std::size_t{w} * 4);
        }
    }
}

/// Draws the PlayStation symbols `all` names (or the key letters `keys` names) in their faces'
/// cells (after Write), from the game's icons as read.
inline void WriteSymbols(const Sheet& sheet, const Image& image, const Icons& icons, u32 all, u32 keys = 0) {
    if ((!all && !keys) || image.bpp != 4) {
        return;
    }
    const u32 scale = image.width / sheet.width, size = CellSize * scale;
    const std::size_t row_bytes = std::size_t{image.width} * 4;
    for (u32 face = 0; face < FaceCount; ++face) {
        const u32 symbol = SymbolOf(all, face);
        const char letter = LetterOf(keys, face);
        if ((!symbol && LetterStrokes(letter).empty()) || !Has(sheet.cells[face])) {
            continue;
        }
        // the cell's top row in the image (rows bottom first): rows then run down by -row_bytes
        const Cell& cell = sheet.cells[face];
        std::vector<u8> top_down(std::size_t{size} * size * 4);
        DrawSymbol(top_down.data(), size, std::size_t{size} * 4, icons[face].data(), symbol, letter);
        for (u32 y = 0; y < size; ++y) {
            const std::size_t row = image.height - 1 - (cell.y * scale + y);
            std::memcpy(image.pixels + (row * image.width + cell.x * scale) * 4, top_down.data() + std::size_t{y} * size * 4, std::size_t{size} * 4);
        }
        (void)row_bytes;
    }
}

/// `text` with the face buttons' 3DS names (A, B, X and Y as words) replaced by the names of the
/// icons shown for them.
inline std::string Rename(std::string_view text, u32 mapping) {
    constexpr std::string_view names = "ABXY";
    const auto word = [&](std::size_t i) {
        const char c = text[i];
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    std::string result(text);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto face = names.find(text[i]);
        if (face != std::string_view::npos && (i == 0 || !word(i - 1)) &&
            (i + 1 == text.size() || !word(i + 1))) {
            result[i] = names[Shown(mapping, static_cast<u32>(face))];
        }
    }
    return result;
}

/// A control reminder's keys for the device in use. `text` names the game's buttons as {A}, {B},
/// {X}, {Y}, {L}, {R}, {ZL}, {ZR}, {Start}, {Select} and the port's own controls as {Touch} (touch
/// mode), {Stick} (the circle pad), {Move} (moving in touch mode), {Tap} and {Leave} (touch mode's
/// finger and exit), {Beams} and {Morph} (the triggers' taps), {Choose} and {Back} (the menu's),
/// {Tabs} and {Close}. Unknown (the emulator-based build): the 3DS names and both devices' keys,
/// the faces as `shown` names them.
inline std::string Keys(std::string_view text) {
    const u32 dev = device.load(std::memory_order_relaxed);
    const NameFn keys = key_name.load(std::memory_order_relaxed);
    const NameFn pad = pad_name.load(std::memory_order_relaxed);
    const NameFn role = role_name.load(std::memory_order_relaxed);
    const bool board = dev == Keyboard, unknown = dev == Unknown || (board ? !keys : (!pad || !role));
    const auto token = [&](std::string_view t) -> std::string {
        static constexpr std::array<std::string_view, 10> controls{"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Start", "Select"};
        for (u32 c = 0; c < controls.size(); ++c) {
            if (t == controls[c]) {
                if (unknown) return c < 4 ? Rename(t, shown.load()) : std::string(t);
                return board ? keys(c) : pad(c);
            }
        }
        if (unknown) {
            if (t == "Touch") return "L3 / Tab";
            if (t == "Stick") return "Circle Pad";
            if (t == "Move") return "D-pad / stick / arrows";
            if (t == "Tap") return Rename("A", shown.load()) + " / Enter";
            if (t == "Leave") return Rename("B", shown.load()) + " / Esc";
            if (t == "Choose") return Rename("A", shown.load());
            if (t == "Back") return Rename("B", shown.load());
            if (t == "Tabs") return "LB / RB";
            if (t == "Close") return "Select or F1";
            if (t == "Beams") return "ZR (right trigger)";
            if (t == "Morph") return "ZL (left trigger)";
            return std::string(t);
        }
        if (board) {
            // the keyboard's roles from its bindings (key_name), these names where it has none
            const auto named = [&](u32 r, const char* fallback) {
                const char* n = keys(r);
                return std::string(n && *n ? n : fallback);
            };
            if (t == "Touch") return named(KeyTouch, "Tab");
            if (t == "Stick") return named(KeyStick, "Arrows");
            if (t == "Move") return named(KeyMove, "Arrows");
            if (t == "Tap") return named(KeyTap, "Enter");
            if (t == "Leave") return named(KeyLeave, "Esc");
            if (t == "Beams") return named(CZR, "?") + " or the mouse wheel";
            if (t == "Morph") return named(CZL, "?");
            if (t == "Choose") return "Enter";
            if (t == "Back") return "Esc";
            if (t == "Tabs") return "PgUp / PgDn";
            if (t == "Close") return "F1";
            return std::string(t);
        }
        // choosing and touch mode's finger on the menus' choosing button, leaving and backing out on
        // their back button (msr-native: the bottom and right ones, Nintendo pads the game's layout)
        if (t == "Touch") return role(PadL3);
        if (t == "Stick") return role(PadStick);
        if (t == "Move") return std::string(role(PadDpad)) + " / " + role(PadStick);
        if (t == "Tap" || t == "Choose") return role(PadChoose);
        if (t == "Leave" || t == "Back") return role(PadCancel);
        if (t == "Tabs") return std::string(role(PadL1)) + " / " + role(PadR1);
        if (t == "Close") return std::string(pad(CSelect)) + " or F1";
        if (t == "Beams") return pad(CZR);
        if (t == "Morph") return pad(CZL);
        return std::string(t);
    };
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '{') {
            const std::size_t end = text.find('}', i);
            if (end != std::string_view::npos) {
                out += token(text.substr(i + 1, end - i - 1));
                i = end + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

} // namespace MSR::ButtonGlyphs
