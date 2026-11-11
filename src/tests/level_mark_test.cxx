// How a fighter's points are laid out from the game's digits (FIGHTER_LEVELS.md,
// Drawing points): LevelMark.hxx lays a number out from the glyphs of one set,
// here a synthetic set whose digits differ in width and height as the game's do.
#include "../ui/LevelMark.hxx"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
namespace point_digits = sf4e::point_digits;
using sf4e::ui::level_mark::Gap;
using sf4e::ui::level_mark::LayOut;
using sf4e::ui::level_mark::Layout;
int failures = 0;
void Check(bool ok, const std::string& what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what.c_str());
}
bool Whole(float value) { return value == std::round(value); }
int Digits(unsigned n) { int count = 1; while (n >= 10) { n /= 10; ++count; } return count; }
// A set whose digit d is 10 + d wide and 20 tall, but the 7 is 24 tall (the
// tallest) and the 0 is 18 tall.
point_digits::Glyphs Glyphs() {
    point_digits::Glyphs glyphs;
    for (int d = 0; d < 10; ++d) glyphs[d] = {d * 20, 40, 10 + d, d == 7 ? 24 : d == 0 ? 18 : 20};
    return glyphs;
}
Layout Lay(unsigned number, float height) {
    static const point_digits::Glyphs glyphs = Glyphs();
    return LayOut(number, height, glyphs);
}
// The points a plate shows: every one-digit number and sample numbers of two to six digits.
std::vector<unsigned> Numbers() {
    std::vector<unsigned> numbers;
    for (unsigned n = 0; n <= 99; ++n) numbers.push_back(n);
    for (const unsigned n : {100u, 101u, 200u, 412u, 700u, 999u, 1500u, 1986u, 2500u, 9999u, 12840u, 99999u, 100000u, 888888u, 999999u}) numbers.push_back(n);
    return numbers;
}
}

int main() {
    const auto glyphs = Glyphs();
    constexpr int Tallest = 24;
    // Every number at a few heights: the tallest digit stands the requested
    // height, the digits sit on one baseline, the ink runs from the origin to
    // the width, and every place is a whole pixel.
    for (const float height : {12.f, 20.f, 24.f, 48.f, 100.f}) {
        const float scale = height / Tallest;
        for (const unsigned number : Numbers()) {
            const auto layout = Lay(number, height);
            const std::string at = "number " + std::to_string(number) + " at " + std::to_string(static_cast<int>(height));
            Check(layout.count == Digits(number), at + ": a glyph per digit");
            Check(std::abs(layout.scale - scale) < 1e-4f, at + ": scaled to the tallest digit");
            Check(layout.width > 0 && Whole(layout.width), at + ": a whole width");
            const auto& first = layout.digits[0];
            const auto& last = layout.digits[layout.count - 1];
            Check(first.x == 0, at + ": the first ink at the origin");
            Check(std::abs(last.x + last.w - layout.width) <= 1.01f, at + ": the last ink ends at the width");
            for (int i = 0; i < layout.count; ++i) {
                const auto& placed = layout.digits[i];
                Check(Whole(placed.x) && Whole(placed.y), at + ": whole pixels");
                Check(std::abs(placed.w - placed.source.w * scale) < 1e-3f && std::abs(placed.h - placed.source.h * scale) < 1e-3f, at + ": the glyph scaled");
                Check(std::abs(placed.y + placed.h - height) <= .51f, at + ": one baseline");
                if (i) {
                    const auto& before = layout.digits[i - 1];
                    Check(std::abs(placed.x - before.x - (before.source.w + Gap) * scale) <= 1.01f, at + ": the game's gap between digits");
                }
            }
        }
        // The tallest digit fills the height from its top.
        const auto seven = Lay(7, height);
        Check(seven.digits[0].y == 0 && std::abs(seven.digits[0].h - height) < 1e-3f, "the tallest digit stands the whole height");
        // The digits are the ones of the number, most significant first.
        const auto number = Lay(1986, height);
        const int expected[] = {1, 9, 8, 6};
        for (int i = 0; i < 4; ++i) Check(number.digits[i].source.x == glyphs[expected[i]].x && number.digits[i].source.w == glyphs[expected[i]].w, "digit " + std::to_string(i) + " of 1986");
        // The same digit repeated is wider with each one: 1, 11, 111 to 111111.
        float before = 0;
        unsigned ones = 0;
        for (int count = 1; count <= 6; ++count) {
            ones = ones * 10 + 1;
            const float width = Lay(ones, height).width;
            Check(width > before, "a number of " + std::to_string(count) + " digits is wider than one of fewer");
            before = width;
        }
    }
    // A number twice as tall is twice as wide, to a pixel.
    for (const unsigned number : Numbers()) {
        const float w24 = Lay(number, 24).width, w48 = Lay(number, 48).width;
        Check(std::abs(w48 - 2 * w24) <= 1.01f, "number " + std::to_string(number) + " scales");
    }
    // Any count of digits; nothing at no height or without glyphs.
    Check(Lay(0, 20).count == 1, "0 is one digit");
    Check(Lay(999999, 40).count == 6, "the most points shown have six digits");
    Check(Lay(4294967295u, 40).count == 10, "the largest number has ten digits");
    Check(Lay(42, 0).count == 0 && Lay(42, -5).width == 0, "nothing at no height");
    Check(LayOut(42, 20, point_digits::Glyphs{}).count == 0, "nothing without glyphs");
    std::printf(failures ? "%d failures\n" : "Level mark: all checks passed\n", failures);
    return failures != 0;
}
