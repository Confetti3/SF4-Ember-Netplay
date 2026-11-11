#pragma once
// Ember's level digits in assets/input/level_digits.png (FIGHTER_LEVELS.md, The
// digits atlas). Written with the atlas by
// scripts/level-digits/make_level_digits.py; check_level_digits.py fails when
// either differs from a new render. Do not edit.
namespace sf4e { namespace level_digits {
// A sprite's place in the atlas, in its pixels.
struct Cell { int x, y, w, h; };
// A digit at one size: its two sprites, `under` (the shadow, red rim, orange
// and yellow spreads) and `core` (the white stroke), the same size and drawn at
// the same place; then, in that size's pixels, the ink's left bearing, the
// ink's width (its lean included) and the advance. A sprite's ink starts `pad`
// in from its left edge.
struct Digit { Cell under, core; int bearing, ink, advance; };
// One size: its cap height, the glow pad round the ink, the baseline's row in
// every sprite of the size, and the digits 0 to 9.
struct Size { int cap, pad, baseline; Digit digits[10]; };
constexpr int AtlasWidth = 1273, AtlasHeight = 586;
// From the smallest cap height.
constexpr int SizeCount = 3;
constexpr Size Sizes[SizeCount] = {
    {20, 10, 33, {
        {{0, 0, 37, 47}, {0, 47, 37, 47}, 1, 17, 16},
        {{37, 0, 31, 47}, {37, 47, 31, 47}, 1, 11, 10},
        {{68, 0, 40, 47}, {68, 47, 40, 47}, 1, 19, 18},
        {{108, 0, 41, 47}, {108, 47, 41, 47}, 1, 21, 19},
        {{149, 0, 39, 47}, {149, 47, 39, 47}, 1, 18, 17},
        {{188, 0, 42, 47}, {188, 47, 42, 47}, 1, 21, 20},
        {{230, 0, 37, 47}, {230, 47, 37, 47}, 1, 16, 15},
        {{267, 0, 39, 47}, {267, 47, 39, 47}, 1, 18, 17},
        {{306, 0, 38, 47}, {306, 47, 38, 47}, 1, 17, 16},
        {{344, 0, 38, 47}, {344, 47, 38, 47}, 1, 18, 16},
    }},
    {40, 16, 61, {
        {{0, 94, 66, 85}, {0, 179, 66, 85}, 1, 34, 31},
        {{66, 94, 54, 85}, {66, 179, 54, 85}, 1, 22, 19},
        {{120, 94, 71, 85}, {120, 179, 71, 85}, 1, 39, 36},
        {{191, 94, 74, 85}, {191, 179, 74, 85}, 1, 41, 39},
        {{265, 94, 69, 85}, {265, 179, 69, 85}, 1, 37, 34},
        {{334, 94, 75, 85}, {334, 179, 75, 85}, 1, 42, 40},
        {{409, 94, 65, 85}, {409, 179, 65, 85}, 1, 33, 30},
        {{474, 94, 69, 85}, {474, 179, 69, 85}, 1, 37, 34},
        {{543, 94, 67, 85}, {543, 179, 67, 85}, 1, 35, 32},
        {{610, 94, 68, 85}, {610, 179, 68, 85}, 1, 35, 33},
    }},
    {80, 28, 117, {
        {{0, 264, 124, 161}, {0, 425, 124, 161}, 2, 67, 62},
        {{124, 264, 100, 161}, {124, 425, 100, 161}, 2, 44, 39},
        {{224, 264, 134, 161}, {224, 425, 134, 161}, 2, 77, 72},
        {{358, 264, 139, 161}, {358, 425, 139, 161}, 2, 83, 78},
        {{497, 264, 130, 161}, {497, 425, 130, 161}, 2, 74, 69},
        {{627, 264, 141, 161}, {627, 425, 141, 161}, 2, 85, 80},
        {{768, 264, 122, 161}, {768, 425, 122, 161}, 2, 66, 61},
        {{890, 264, 130, 161}, {890, 425, 130, 161}, 2, 73, 68},
        {{1020, 264, 126, 161}, {1020, 425, 126, 161}, 2, 69, 64},
        {{1146, 264, 127, 161}, {1146, 425, 127, 161}, 2, 70, 65},
    }},
};
} }
