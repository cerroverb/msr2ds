// Copyright 2026 Samus Returns port
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Classifies what Samus Returns shows on the bottom screen from a 320x240 readback of it.
// Measured on the USA release:
//  - gameplay HUD: cyan rules under the energy bar (y 40) and above the counters (y 215), blue
//    level ~165; during cutscenes the game dims the same HUD to ~95-110 (hidden);
//  - framed menus (pause pages, file select): a horizontal cyan rule of 100 px or more; the title
//    menus (file select, gallery...) are drawn without their backdrop, so on black;
//  - prompts (title, prologue): text brighter than 140 on the bare backdrop;
//  - the idle emblem (cutscenes, loading): nothing brighter than ~110.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include "msr/presentation.h"

namespace MSR::BottomClassifier {

inline constexpr int Width = 320;
inline constexpr int Height = 240;

struct Image {
    const std::uint8_t* pixels; ///< Width * Height * 4 bytes, rows top to bottom
    bool bgra;

    void Rgb(int x, int y, int& r, int& g, int& b) const {
        const std::uint8_t* p = pixels + (static_cast<std::size_t>(y) * Width + x) * 4;
        r = bgra ? p[2] : p[0];
        g = p[1];
        b = bgra ? p[0] : p[2];
    }
    int Brightness(int x, int y) const {
        int r, g, b;
        Rgb(x, y, r, g, b);
        return std::max(r, std::max(g, b));
    }
};

/// A HUD rule at row `y`: the fraction of x where blue in rows y+-2 stands out from the rows 7
/// above and below, and the rule's mean blue level. Contrast finds the rules even when the
/// game dims the bottom screen for a cutscene.
struct Rule {
    float coverage;
    float level;
};
inline Rule FindRule(const Image& image, int y, int x0, int x1) {
    int hits = 0;
    float level = 0.f;
    for (int x = x0; x < x1; ++x) {
        int line = 0;
        for (int dy = -2; dy <= 2; ++dy) {
            int r, g, b;
            image.Rgb(x, std::clamp(y + dy, 0, Height - 1), r, g, b);
            line = std::max(line, b);
        }
        int r, g, above, below;
        image.Rgb(x, std::clamp(y - 7, 0, Height - 1), r, g, above);
        image.Rgb(x, std::clamp(y + 7, 0, Height - 1), r, g, below);
        hits += line - std::min(above, below) > 35;
        level += static_cast<float>(line);
    }
    const float count = static_cast<float>(std::max(1, x1 - x0));
    return {static_cast<float>(hits) / count, level / count};
}

/// A cyan pixel: blue at least `min_blue` and `min_tint` above red, green above red.
inline bool Cyan(const Image& image, int x, int y, int min_blue, int min_tint) {
    int r, g, b;
    image.Rgb(x, y, r, g, b);
    return b >= min_blue && b > r + min_tint && g > r;
}

/// Longest horizontal run of bright cyan pixels in rows [y0, y1). Framed menus have long rules;
/// the title, prologue and emblem have none. A lower `min_blue` also finds unlit frames.
struct Run {
    int length = 0;
    int y = 0;
    int x0 = 0;
    int x1 = 0;
};
inline Run LongestRule(const Image& image, int y0 = 0, int y1 = Height, int min_blue = 150,
                       int min_tint = 30) {
    Run best;
    for (int y = y0; y < y1; ++y) {
        int run = 0;
        for (int x = 0; x < Width; ++x) {
            run = Cyan(image, x, y, min_blue, min_tint) ? run + 1 : 0;
            if (run > best.length) {
                best = {run, y, x + 1 - run, x + 1};
            }
        }
    }
    return best;
}

/// Longest vertical run of bright cyan pixels in columns [x0, x1), rows [y0, y1).
inline int LongestColumn(const Image& image, int x0, int x1, int y0, int y1) {
    int best = 0;
    for (int x = x0; x < x1; ++x) {
        int run = 0;
        for (int y = y0; y < y1; ++y) {
            run = Cyan(image, x, y, 150, 30) ? run + 1 : 0;
            best = std::max(best, run);
        }
    }
    return best;
}

/// Fraction of pixels brighter than `threshold`, sampled every other pixel.
inline float BrightFraction(const Image& image, int threshold) {
    int hits = 0;
    int total = 0;
    for (int y = 0; y < Height; y += 2) {
        for (int x = 0; x < Width; x += 2) {
            hits += image.Brightness(x, y) > threshold;
            ++total;
        }
    }
    return static_cast<float>(hits) / static_cast<float>(total);
}

/// The box around the pixels brighter than `threshold` (a prompt's text), x1 and y1 exclusive;
/// empty when there are none.
struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool Empty() const {
        return x1 <= x0;
    }
};
inline Box BrightBox(const Image& image, int threshold) {
    Box box{Width, Height, 0, 0};
    for (int y = 0; y < Height; ++y) {
        for (int x = 0; x < Width; ++x) {
            if (image.Brightness(x, y) > threshold) {
                box = {std::min(box.x0, x), std::min(box.y0, y), std::max(box.x1, x + 1),
                       std::max(box.y1, y + 1)};
            }
        }
    }
    return box.x1 > box.x0 ? box : Box{};
}

/// Fraction of pixels darker than `threshold`, sampled every other pixel.
inline float DarkFraction(const Image& image, int threshold) {
    int hits = 0;
    int total = 0;
    for (int y = 0; y < Height; y += 2) {
        for (int x = 0; x < Width; x += 2) {
            hits += image.Brightness(x, y) < threshold;
            ++total;
        }
    }
    return static_cast<float>(hits) / static_cast<float>(total);
}

/// Brightest pixel, sampled every other pixel (0..255).
inline int Peak(const Image& image) {
    int peak = 0;
    for (int y = 0; y < Height; y += 2) {
        for (int x = 0; x < Width; x += 2) {
            peak = std::max(peak, image.Brightness(x, y));
        }
    }
    return peak;
}

/// Title menus. The gallery viewer frames its page counter with a cyan edge down each side
/// (x 35 and 284). The file select's first file box starts at y ~33; in the list the box closes
/// at y ~85, an opened file's box runs on around its buttons. Unlit boxes are dimmer.
inline Presentation::Page TitleMenuPage(const Image& image) {
    using Presentation::Page;
    if (LongestColumn(image, 30, 42, 50, 190) >= 100 &&
        LongestColumn(image, 278, 290, 50, 190) >= 100) {
        return Page::Gallery;
    }
    const Run box_top = LongestRule(image, 28, 39, 70, 15);
    if (box_top.length >= 240 && box_top.x0 <= 55) {
        const Run box_bottom = LongestRule(image, 80, 90, 70, 15);
        return box_bottom.length >= 240 && box_bottom.x0 <= 45 ? Page::FileList : Page::FileOpen;
    }
    return Page::Other;
}

/// The HUD's beam slots (Presentation::beam_slot): their rectangles, which one is lit (about a
/// quarter of a lit slot is brighter than 200, none of an unlit one) and which hold an icon. An
/// icon stands out from the slot's middle (brightest minus the darkest fifth: 59-103), an empty
/// slot does not (5-26; the empty Grapple slot is a plain striped fill); a lit icon can fill the
/// middle entirely, but a lit slot is never empty. One slot is always lit, reaching 255. In Morph
/// Ball the game greys the whole row out: the lit slot dims (brightest 156-216), sometimes below
/// the lit test, and the unlit icons fade into their slots (30-34, read as empty). `greyed`
/// says so: no slot is lit at full brightness.
inline constexpr std::array<std::array<int, 4>, 3> BeamSlotRects = {{
    {253, 13, 317, 77},   // Aeion ability: the usual beam
    {253, 88, 317, 152},  // Ice Beam
    {253, 163, 317, 227}, // Grapple Beam
}};
struct BeamSlots {
    int lit = -1;
    int present = 0;
    bool greyed = false;
};
inline BeamSlots ReadBeamSlots(const Image& image) {
    BeamSlots beams;
    int lit_peak = 0;
    for (int i = 0; i < static_cast<int>(BeamSlotRects.size()); ++i) {
        const auto& [x0, y0, x1, y1] = BeamSlotRects[i];
        int bright = 0, total = 0, peak = 0;
        for (int y = y0; y < y1; y += 2) {
            for (int x = x0; x < x1; x += 2) {
                const int brightness = image.Brightness(x, y);
                bright += brightness > 200;
                peak = std::max(peak, brightness);
                ++total;
            }
        }
        std::array<int, 144> middle{};
        std::size_t count = 0;
        for (int y = (y0 + y1) / 2 - 12; y < (y0 + y1) / 2 + 12; y += 2) {
            for (int x = (x0 + x1) / 2 - 12; x < (x0 + x1) / 2 + 12; x += 2) {
                middle[count++] = image.Brightness(x, y);
            }
        }
        const int brightest = *std::max_element(middle.begin(), middle.begin() + count);
        std::nth_element(middle.begin(), middle.begin() + count / 5, middle.begin() + count);
        const bool lit = bright * 10 > total;
        if (lit) {
            beams.lit = i;
            lit_peak = peak;
        }
        if (lit || brightest - middle[count / 5] > 40) {
            beams.present |= 1 << i;
        }
    }
    beams.greyed = lit_peak < 240;
    return beams;
}

/// The HUD's map, clear of the HUD's frames: under the energy bar and the late game's Aeion
/// gauge and ability selector (y 55), above the counters' rule (y 213), left of the beam slots
/// (x 253), where the game fades its map out.
inline constexpr std::array<int, 4> MapRect = {0, 58, 252, 213};

/// Samus's marker on the HUD's map: a red dot with a white middle, 8 px across, that fades out
/// and back each beat (gone for about a third of a second); in a save station it turns to an
/// orange helmet by turns. The game keeps it in the middle of its map (x 125, y 135) but stops
/// scrolling near an area's edges, where the marker moves instead, a map tile (10 px) at a time.
/// The red and orange pixels are counted in 4 px cells, and the marker is the 12 px square
/// holding most: about 42 when fully lit, under 32 while it fades (and its edges with it). Not
/// found when it is faint or something else on the map is as red.
struct Marker {
    bool found = false;
    float x = 0.f, y = 0.f; ///< Its middle
};
inline Marker FindMapMarker(const Image& image) {
    const auto& [x0, y0, x1, y1] = MapRect;
    constexpr int cell = 4;
    constexpr int columns = (MapRect[2] - MapRect[0] + cell - 1) / cell;
    constexpr int rows = (MapRect[3] - MapRect[1] + cell - 1) / cell;
    const auto marker = [&image](int x, int y) {
        int r, g, b;
        image.Rgb(x, y, r, g, b);
        const bool red = r >= 0x60 && g * 5 <= r * 2 && b * 5 <= r * 2;
        const bool visor = r >= 0xc0 && g * 20 >= r * 9 && g * 4 <= r * 3 && b * 10 <= r * 3;
        return red || visor;
    };
    std::array<int, columns * rows> cells{};
    int total = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (marker(x, y)) {
                ++cells[(y - y0) / cell * columns + (x - x0) / cell];
                ++total;
            }
        }
    }
    int best = 0, best_column = 0, best_row = 0;
    for (int row = 0; row + 3 <= rows; ++row) {
        for (int column = 0; column + 3 <= columns; ++column) {
            int count = 0;
            for (int j = 0; j < 3; ++j) {
                for (int i = 0; i < 3; ++i) {
                    count += cells[(row + j) * columns + column + i];
                }
            }
            if (count > best) {
                best = count;
                best_column = column;
                best_row = row;
            }
        }
    }
    if (best < 32 || total - best > 24) {
        return {};
    }
    // Its middle: the box around its pixels in that square.
    const int sx0 = x0 + best_column * cell, sy0 = y0 + best_row * cell;
    const int sx1 = std::min(x1, sx0 + 3 * cell), sy1 = std::min(y1, sy0 + 3 * cell);
    Box box{sx1, sy1, sx0, sy0};
    for (int y = sy0; y < sy1; ++y) {
        for (int x = sx0; x < sx1; ++x) {
            if (marker(x, y)) {
                box = {std::min(box.x0, x), std::min(box.y0, y), std::max(box.x1, x + 1),
                       std::max(box.y1, y + 1)};
            }
        }
    }
    return {true, static_cast<float>(box.x0 + box.x1) / 2.f,
            static_cast<float>(box.y0 + box.y1) / 2.f};
}

/// The map's backdrop in a strip along one of its edges: the mean colour of the pixels no
/// brighter than the median and a little (its scanline stripes), which leaves out rooms and
/// icons when they cover less than half of it. Channels 0..255, red first.
inline std::array<int, 3> Backdrop(const Image& image, int x0, int y0, int x1, int y1) {
    std::array<int, 512> levels{};
    std::size_t count = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1 && count < levels.size(); ++x) {
            levels[count++] = image.Brightness(x, y);
        }
    }
    if (count == 0) {
        return {};
    }
    std::nth_element(levels.begin(), levels.begin() + count / 2, levels.begin() + count);
    const int limit = levels[count / 2] + 8;
    std::array<int, 3> sum{};
    int used = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            int r, g, b;
            image.Rgb(x, y, r, g, b);
            if (std::max(r, std::max(g, b)) <= limit) {
                sum = {sum[0] + r, sum[1] + g, sum[2] + b};
                ++used;
            }
        }
    }
    return {sum[0] / used, sum[1] / used, sum[2] / used};
}

struct Result {
    Presentation::Bottom bottom;
    Presentation::Page page = Presentation::Page::Other;
    BeamSlots beams{}; ///< When the HUD is up
    Marker marker{};   ///< When the HUD is up
};

inline Result Classify(const Image& image) {
    using Presentation::Bottom;
    using Presentation::Page;
    // Gameplay HUD: rules under the energy bar and above the counters. Bright when playing,
    // dimmed by the game during cutscenes, when it stays hidden. A menu's title rule (y ~24,
    // 100 px or more; the HUD has 23 at most there) rules it out: a well explored pause map
    // draws its outlines along the HUD's rule.
    const Rule top = FindRule(image, 40, 85, 220);
    const Rule bottom = FindRule(image, 215, 65, 230);
    const Run title = LongestRule(image, 18, 31);
    if (top.coverage > .8f && bottom.coverage > .8f && title.length < 100) {
        if (top.level > 135.f && bottom.level > 135.f) {
            return {Bottom::Hud, Page::Other, ReadBeamSlots(image), FindMapMarker(image)};
        }
        return {Bottom::Logo};
    }
    // Framed menus have a long rule. The title menus stand on black, the pause menu on its
    // backdrop; its pages are told apart by the rule under their title (y ~24): map page
    // x 61-259, Samus page x 61-320, map pins x 101-219.
    if (LongestRule(image).length >= 100) {
        if (DarkFraction(image, 6) > .3f) {
            return {Bottom::Menu, TitleMenuPage(image)};
        }
        Page page = Page::Other;
        if (title.length >= 60) {
            if (title.x1 >= 310 && title.x0 < 90) {
                page = Page::PauseSamus;
            } else if (title.x0 < 80 && title.x1 < 280 && bottom.coverage > .8f) {
                page = Page::PauseMap;
            } else if (title.x0 >= 90 && title.x1 <= 235 && bottom.coverage > .8f) {
                page = Page::MapPins;
            }
        }
        return {Bottom::Menu, page};
    }
    if (BrightFraction(image, 140) > 0.001f) {
        return {Bottom::Prompt};
    }
    return {Bottom::Logo};
}

/// Writes the readback as a binary PPM (developer calibration).
inline void WritePpm(const Image& image, const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", Width, Height);
    for (int y = 0; y < Height; ++y) {
        for (int x = 0; x < Width; ++x) {
            int r, g, b;
            image.Rgb(x, y, r, g, b);
            const std::uint8_t rgb[3] = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                         static_cast<std::uint8_t>(b)};
            std::fwrite(rgb, 1, 3, file);
        }
    }
    std::fclose(file);
}

} // namespace MSR::BottomClassifier
