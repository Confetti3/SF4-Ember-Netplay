#pragma once
// The digits of the game's ranked result screen, which Ember draws fighter
// points with: the sheet net_vs_number.dds inside
// resource/ui/net_result/CMN/net_result.tex.emz of the player's own game, read
// at runtime and never shipped. The sheet is 256 by 256 and holds three sets
// of 0 to 9 (white, and the gold "rank up" and blue "rank down" sets) in rows
// whose places are known only roughly (kBands); each glyph's ink box is
// measured from the sheet's alpha, and a sheet whose rows are not found as
// expected is refused, so a changed game file draws the heading font instead.
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace sf4e { namespace point_digits {
constexpr int SheetSide = 256;
enum Set { White, Gold, Blue, SetCount };
// A glyph's ink in the sheet's pixels, and the rows of its face: the light
// fill inside the dark outline and shadow, which is what reads as the digit's
// height (faceH 0 when the glyph has no light fill, as a test sheet has not).
struct Box { int x = 0, y = 0, w = 0, h = 0, faceY = 0, faceH = 0; };
using Glyphs = std::array<Box, 10>;
struct Sheet { std::array<Glyphs, SetCount> sets; };

namespace detail {
// A row of glyphs: the sheet rows it spans, then, left to right, `skip` runs
// of ink that are not digits (the arrow), `count` digits from `first`, and
// `extra` more that are not digits (the point and the percent sign).
struct Band { Set set; int top, bottom, first, skip, count, extra; };
constexpr Band kBands[] = {
    {Gold, 3, 29, 0, 1, 5, 0},    {Gold, 37, 61, 5, 0, 5, 0},
    {Blue, 72, 97, 0, 1, 5, 0},   {Blue, 105, 129, 5, 0, 5, 0},
    {White, 143, 173, 0, 0, 6, 0}, {White, 183, 213, 6, 0, 4, 2},
};
// The rows scanned reach this far past a band, which the rows between bands
// leave clear.
constexpr int Slack = 2;
// Alpha below this is not ink.
constexpr unsigned char MinimumAlpha = 16;
// A face pixel: this opaque, and its brightest channel at least this light
// (the white and gold sets' fills; the outline and shadow are near black).
constexpr unsigned char FaceAlpha = 128, FaceLight = 128;
constexpr int MinimumWidth = 3, MinimumHeight = 10;
}

// Measures `bgra` (width by height, 4 bytes a pixel, rows packed). False, with
// `why`, when the sheet is not SheetSide square or a row does not hold the runs
// of ink it should; `sheet` is then unspecified.
inline bool Measure(const unsigned char* bgra, int width, int height, Sheet& sheet, std::string& why) {
    if (!bgra || width != SheetSide || height != SheetSide) { why = "not " + std::to_string(SheetSide) + "x" + std::to_string(SheetSide); return false; }
    const auto ink = [&](int x, int y) { return bgra[(y * width + x) * 4 + 3] >= detail::MinimumAlpha; };
    for (const auto& band : detail::kBands) {
        const int top = band.top - detail::Slack, bottom = band.bottom + detail::Slack;
        // Runs of columns that hold ink in the band's rows.
        std::vector<Box> runs;
        bool open = false;
        for (int x = 0; x < width; ++x) {
            int low = height, high = -1;
            for (int y = top; y <= bottom; ++y) if (ink(x, y)) { if (y < low) low = y; high = y; }
            if (high < 0) { open = false; continue; }
            if (!open) { runs.push_back({x, low, 1, high - low + 1}); open = true; continue; }
            Box& run = runs.back();
            const int last = (std::max)(run.y + run.h - 1, high);
            run.w = x - run.x + 1; run.y = (std::min)(run.y, low); run.h = last - run.y + 1;
        }
        if (static_cast<int>(runs.size()) != band.skip + band.count + band.extra) {
            why = "row " + std::to_string(band.top) + " holds " + std::to_string(runs.size()) + " glyphs, not " + std::to_string(band.skip + band.count + band.extra);
            return false;
        }
        for (int i = 0; i < band.count; ++i) {
            const Box& glyph = runs[band.skip + i];
            if (glyph.w < detail::MinimumWidth || glyph.h < detail::MinimumHeight) { why = "a glyph of row " + std::to_string(band.top) + " is too small"; return false; }
            Box& placed = sheet.sets[band.set][band.first + i];
            placed = glyph;
            // The face's rows inside the ink box.
            int faceTop = -1, faceBottom = -1;
            for (int y = glyph.y; y < glyph.y + glyph.h; ++y)
                for (int x = glyph.x; x < glyph.x + glyph.w; ++x) {
                    const unsigned char* p = bgra + (y * width + x) * 4;
                    if (p[3] < detail::FaceAlpha || (std::max)({p[0], p[1], p[2]}) < detail::FaceLight) continue;
                    if (faceTop < 0) faceTop = y;
                    faceBottom = y;
                    break;
                }
            if (faceTop >= 0) { placed.faceY = faceTop; placed.faceH = faceBottom - faceTop + 1; }
        }
    }
    return true;
}
} }
