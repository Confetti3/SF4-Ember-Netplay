// Ember's level digits (FIGHTER_LEVELS.md, The digits atlas): the atlas's
// layout in src/common/LevelDigits.hxx and how LevelMark.hxx lays a number out
// from it, for every level 1 to 60 at each rendered size and between them.
#include "../ui/LevelMark.hxx"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
namespace level_digits = sf4e::level_digits;
using level_digits::Cell;
int failures = 0;
void Check(bool ok, const std::string& what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}
bool Whole(float value) { return value == std::round(value); }
bool Inside(const Cell& c) { return c.x >= 0 && c.y >= 0 && c.w > 0 && c.h > 0 && c.x + c.w <= level_digits::AtlasWidth && c.y + c.h <= level_digits::AtlasHeight; }
bool Apart(const Cell& a, const Cell& b) { return a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y; }
int Digits(unsigned n) { int count = 1; while (n >= 10) { n /= 10; ++count; } return count; }
}

int main() {
    using sf4e::ui::level_mark::LayOut;
    // The sizes, smallest first, each sprite in the atlas and none over another.
    const int caps[] = {20, 40, 80};
    Check(level_digits::SizeCount == 3, "three sizes");
    std::vector<Cell> cells;
    for (int s = 0; s < level_digits::SizeCount; ++s) {
        const auto& size = level_digits::Sizes[s];
        Check(size.cap == caps[s], "cap heights 20, 40, 80");
        Check(size.pad == static_cast<int>(size.cap * .3) + 4, "the glow pad is 0.3 of the cap and 4");
        for (const auto& digit : size.digits) {
            Check(Inside(digit.under) && Inside(digit.core), "sprites inside the atlas");
            Check(digit.under.w == digit.core.w && digit.under.h == digit.core.h, "a digit's two sprites are the same size");
            Check(digit.under.w == digit.ink + 2 * size.pad || digit.under.w == digit.ink + 2 * size.pad + 1, "a sprite is its ink and the pad each side");
            Check(size.baseline > size.cap && size.baseline < digit.under.h, "the baseline is inside the sprite, under the cap");
            Check(digit.advance > 0 && digit.advance < digit.ink + 2 * digit.bearing, "the next digit tucks under the lean");
            cells.push_back(digit.under);
            cells.push_back(digit.core);
        }
    }
    for (std::size_t i = 0; i < cells.size(); ++i)
        for (std::size_t j = i + 1; j < cells.size(); ++j) Check(Apart(cells[i], cells[j]), "sprites do not overlap");

    // Every level at each size: drawn from that size unscaled, its first ink at
    // the origin, its last ink ending at its width, every place a whole pixel.
    for (const int cap : caps) {
        float widest = 0;
        for (unsigned level = 1; level <= 60; ++level) {
            const auto layout = LayOut(level, static_cast<float>(cap));
            const std::string at = "level " + std::to_string(level) + " at " + std::to_string(cap);
            Check(layout.size && layout.size->cap == cap && layout.scale == 1, at + ": its own size, unscaled");
            Check(layout.count == Digits(level), at + ": a sprite pair per digit");
            Check(layout.width > 0 && Whole(layout.width), at + ": a whole width");
            const auto& first = layout.digits[0];
            const auto& last = layout.digits[layout.count - 1];
            Check(first.x == -layout.size->pad, at + ": the first ink at the origin");
            Check(std::abs(last.x + layout.size->pad + last.digit->ink - layout.width) <= 1.01f, at + ": the last ink ends at the width");
            for (int i = 0; i < layout.count; ++i) {
                const auto& placed = layout.digits[i];
                Check(Whole(placed.x) && Whole(placed.y), at + ": whole pixels");
                Check(placed.y == layout.size->cap - layout.size->baseline, at + ": the baseline under the cap");
                if (i) Check(placed.x > layout.digits[i - 1].x && placed.x < layout.digits[i - 1].x + layout.digits[i - 1].w, at + ": digits step right and tuck");
            }
            widest = (std::max)(widest, layout.width);
        }
        // Two digits never take more than two and a half caps, so a plate's width is foreseeable.
        Check(widest <= cap * 2.5f, "60 levels fit in two and a half caps at " + std::to_string(cap));
    }
    // The sizes agree: a number twice as tall is twice as wide, to a couple of pixels.
    for (unsigned level = 1; level <= 60; ++level) {
        const float w20 = LayOut(level, 20).width, w40 = LayOut(level, 40).width, w80 = LayOut(level, 80).width;
        Check(std::abs(w40 - 2 * w20) <= 2 && std::abs(w80 - 2 * w40) <= 2, "level " + std::to_string(level) + " scales between sizes");
    }
    // Between sizes: the smallest size at least as tall, scaled down by at
    // most half from 11 px; above 80 the largest scaled up. Still whole
    // pixels, the ink still from the origin to the width.
    for (int tall = 8; tall <= 160; ++tall) {
        const float height = static_cast<float>(tall);
        const int expected = tall <= 20 ? 20 : tall <= 40 ? 40 : 80;
        for (unsigned level = 1; level <= 60; ++level) {
            const auto layout = LayOut(level, height);
            const std::string at = "level " + std::to_string(level) + " at " + std::to_string(tall);
            Check(layout.size && layout.size->cap == expected && layout.scale == height / expected, at + ": the size at least as tall");
            Check(tall <= 10 || layout.scale > .5f, at + ": scaled down by at most half");
            const auto& first = layout.digits[0];
            const auto& last = layout.digits[layout.count - 1];
            Check(Whole(layout.width) && Whole(first.x) && Whole(first.y), at + ": whole pixels");
            Check(std::abs(first.x + layout.size->pad * layout.scale) <= 1.01f, at + ": the first ink at the origin");
            Check(std::abs(last.x + (layout.size->pad + last.digit->ink) * layout.scale - layout.width) <= 1.01f, at + ": the last ink ends at the width");
        }
    }
    // Any count of digits; nothing at no height.
    // Sizes has internal linkage, so each file holds its own copy: the digit is
    // told by its sprite's place in the atlas, not by its address.
    const auto zero = LayOut(0, 20);
    const Cell& zeroUnder = level_digits::Sizes[0].digits[0].under;
    Check(zero.count == 1 && zero.digits[0].digit->under.x == zeroUnder.x && zero.digits[0].digit->under.y == zeroUnder.y, "0 is one digit");
    Check(LayOut(4294967295u, 40).count == 10, "the largest number has ten digits");
    Check(!LayOut(42, 0).size && LayOut(42, 0).count == 0 && LayOut(42, -5).width == 0, "nothing at no height");
    std::printf(failures ? "%d failures\n" : "Level mark: all checks passed\n", failures);
    return failures != 0;
}
