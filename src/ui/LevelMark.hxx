#pragma once
// A number in the game's own point digits (FIGHTER_LEVELS.md, Drawing points):
// laid out from the glyphs of one set of the ranked result screen's sheet
// (src/common/PointDigits.hxx). Only the layout is here, so it can be checked
// without a device; the parts that draw it are v2::Level, v2::LevelGain and
// v2::LevelBand (V2.hxx), implemented in LevelMark.cxx.
#include "../common/PointDigits.hxx"
#include <array>

namespace sf4e { namespace ui { namespace level_mark {
// The gap between neighbouring digits' ink, in the sheet's pixels.
constexpr int Gap = 2;
// A digit placed: its ink box in the sheet, and where and how big it is drawn,
// in drawn pixels from the number's origin (the left of its first digit's ink,
// level with the top of the tallest digit). x and y are whole pixels.
struct Placed { point_digits::Box source; float x = 0, y = 0, w = 0, h = 0; };
// A number whose tallest digit is `height` tall, its digits left to right on
// one baseline (the bottom of their ink). `width`, in whole pixels, runs from
// the first digit's ink to the last's.
struct Layout {
    float scale = 0, width = 0;
    int count = 0;
    std::array<Placed, 10> digits{};
};
// Nothing for a height of 0 or less.
Layout LayOut(unsigned number, float height, const point_digits::Glyphs& glyphs);
} } }
