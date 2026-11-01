// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The game's face button icons (A, B, X, Y) as an Xbox pad labels the buttons they are bound to.
// The game draws them from two textures: its font sheet (the icons inside text, "Press Ⓐ to
// Start") and its interface atlas (the same icons as sprites). With the usual bindings the 3DS A
// is the right face button, which Xbox labels B, so the A icon shows the game's own B icon, and
// so on: the renderer copies the icon cells within each texture as it uploads it
// (rasterizer_cache.h), and the in-game menu names the buttons the same way. Other pads keep the
// game's icons. Header-only and inert while `shown` is the identity, like msr/texture_packs.h.

#pragma once

#include <array>
#include <atomic>
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

/// A face icon's cell, in the texture as dumped (top row first). All are 13 x 13.
struct Cell {
    u32 x, y;
};
inline constexpr u32 CellSize = 13;

struct Sheet {
    u64 hash; ///< CityHash64 of the texture's data in emulated memory (Azahar's "new" hash)
    u32 width, height;
    u32 format; ///< VideoCore::PixelFormat
    std::array<Cell, FaceCount> cells;
};

/// Samus Returns (USA).
inline constexpr std::array<Sheet, 2> Sheets{{
    // The font sheet (IA8), several sizes of text and the button icons.
    {0x5223C28BC4470EB0, 512, 256, 5, {{{23, 204}, {87, 143}, {25, 221}, {105, 139}}}},
    // The interface atlas (I8): the same icons beside the D-pad, L, R and START sprites.
    {0xB0F410A02C69F9AB, 1024, 512, 7, {{{706, 66}, {691, 66}, {706, 51}, {691, 51}}}},
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

/// Calls `copy(row in the image, row in the icon, bytes)` for each row of each face's cell.
template <class Copy>
void ForEachRow(const Sheet& sheet, const Image& image, Copy&& copy) {
    const u32 scale = image.width / sheet.width;
    const u32 size = CellSize * scale;
    const std::size_t row_bytes = std::size_t{size} * image.bpp;
    for (u32 face = 0; face < FaceCount; ++face) {
        const Cell& cell = sheet.cells[face];
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

/// Draws in each face's cell the icon `mapping` shows for it.
inline void Write(const Sheet& sheet, const Image& image, const Icons& icons, u32 mapping) {
    ForEachRow(sheet, image, [&](u32 face, u8* row, std::size_t offset, std::size_t bytes) {
        std::memcpy(row, icons[Shown(mapping, face)].data() + offset, bytes);
    });
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

} // namespace MSR::ButtonGlyphs
