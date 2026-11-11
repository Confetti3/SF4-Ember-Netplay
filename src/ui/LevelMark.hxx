#pragma once
// A number in Ember's level digits (FIGHTER_LEVELS.md, The digits atlas), laid
// out from the sprites src/common/LevelDigits.hxx places in the atlas. Only the
// layout is here, so it can be checked without a device; the parts that draw
// it are v2::Level and v2::LevelBand (V2.hxx), implemented in LevelMark.cxx.
#include "../common/LevelDigits.hxx"
#include <array>

namespace sf4e { namespace ui { namespace level_mark {
// A digit placed: its sprites, and where and how big both are drawn, in drawn
// pixels from the number's origin (the left of its first digit's ink, level
// with the top of its cap). x and y are whole pixels.
struct Placed { const level_digits::Digit* digit = nullptr; float x = 0, y = 0, w = 0, h = 0; };
// A number `height` tall from the cap's top to the baseline: drawn from the
// smallest atlas size at least that tall (the largest above 80), scaled by
// `scale`, its digits left to right. `width`, in whole pixels, runs from the
// first digit's ink to the last's, its lean included, so the number centres on
// what is painted; the glow reaches the size's pad past every side of that.
struct Layout {
    const level_digits::Size* size = nullptr;
    float scale = 0, width = 0;
    int count = 0;
    std::array<Placed, 10> digits{};
};
// Nothing for a height of 0 or less.
Layout LayOut(unsigned number, float height);
} } }
