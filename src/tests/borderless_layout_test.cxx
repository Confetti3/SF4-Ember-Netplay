#include "../common/BorderlessLayout.hxx"
#include "test_support.hxx"
#include <climits>

int main() {
    using sf4e::display::Fit16By9;
    const auto ultrawide = Fit16By9(3440, 1440);
    CHECK(ultrawide.width == 2560 && ultrawide.height == 1440);
    CHECK(ultrawide.x == 440 && ultrawide.y == 0);
    const auto uhd = Fit16By9(3840, 2160);
    CHECK(uhd.width == 3840 && uhd.height == 2160 && uhd.x == 0 && uhd.y == 0);
    const auto tall = Fit16By9(1920, 1200);
    CHECK(tall.width == 1920 && tall.height == 1080 && tall.y == 60);
    CHECK(!Fit16By9(0, 1440).Valid());
    CHECK(!Fit16By9(-3440, 1440).Valid());
    CHECK(!Fit16By9(16, 8).Valid());
    // Portrait, fractional desktop dimensions and large inputs must never
    // crop, overflow or lose aspect ratio; spare odd pixels go right/bottom.
    const int sizes[][2] = {{1080, 1920}, {1366, 768}, {3441, 1441}, {INT_MAX, INT_MAX}};
    for (const auto& size : sizes) {
        const auto fit = Fit16By9(size[0], size[1]);
        CHECK(fit.Valid());
        CHECK(fit.width / 16 == fit.height / 9);
        CHECK(fit.x >= 0 && fit.y >= 0);
        CHECK(fit.x + fit.width <= size[0] && fit.y + fit.height <= size[1]);
        CHECK(size[0] - fit.width - 2 * fit.x <= 1);
        CHECK(size[1] - fit.height - 2 * fit.y <= 1);
    }
}
