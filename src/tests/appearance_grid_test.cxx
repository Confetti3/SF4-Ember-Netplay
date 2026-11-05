// The costume, colour and Ultra grids' layout (AppearanceGrid.hxx): every card at
// once and as large as fits, a short set on one line, a long one in a block
// that fits the height, and a set too large for the area held at the
// narrowest card, to scroll.
#include "../ui/AppearanceGrid.hxx"
#include "test_support.hxx"
#include <iostream>

int main() {
    using namespace sf4e::ui;
    constexpr float gap = 8, aspect = 1.3f, caption = 24, narrowest = 64, widest = 220;
    const auto fits = [&](const GalleryGrid& grid, std::size_t cards, float width, float height) {
        const auto last = grid.Cell(cards - 1, width, gap);
        return last.x + grid.width <= width + .01f && last.y + grid.height <= height + .01f;
    };

    // Seven costumes on a wide page: one line, each at the widest a card goes.
    {
        const auto grid = FitGallery(7, 1800, 700, gap, aspect, caption, narrowest, widest);
        CHECK(grid.columns == 7 && grid.width == widest && grid.Lines(7) == 1);
        CHECK(grid.height - (widest * aspect + caption) < .01f && grid.height - (widest * aspect + caption) > -.01f);
        CHECK(fits(grid, 7, 1800, 700));
        // The line is centred across the page.
        const auto first = grid.Cell(0, 1800, gap), last = grid.Cell(6, 1800, gap);
        CHECK(first.y == 0 && last.y == 0);
        CHECK(first.x > 0 && first.x - (1800 - (last.x + grid.width)) < .01f && first.x - (1800 - (last.x + grid.width)) > -.01f);
    }
    // Twenty-two colours in a 1280x600 body: every one shows, none beyond the
    // area, and no other column count would make them larger.
    {
        const auto grid = FitGallery(22, 1280, 600, gap, aspect, caption, narrowest, widest);
        CHECK(grid.columns > 1 && grid.columns < 22);
        CHECK(grid.width >= narrowest && grid.width <= widest);
        CHECK(fits(grid, 22, 1280, 600));
        for (std::size_t columns = 1; columns <= 22; ++columns) {
            const std::size_t lines = (22 + columns - 1) / columns;
            const float across = (1280 - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
            const float room = (600 - gap * static_cast<float>(lines - 1)) / static_cast<float>(lines);
            const float w = (std::min)({widest, across, (room - caption) / aspect});
            CHECK(w <= grid.width + .001f);
        }
        // Cards go line by line: the next line starts under the first card.
        const auto next = grid.Cell(static_cast<std::size_t>(grid.columns), 1280, gap), first = grid.Cell(0, 1280, gap);
        CHECK(next.x == first.x && next.y == grid.height + gap);
    }
    // A short, tall area: the height decides, and of the column counts that
    // give the same size the fewest lines win, then the fewest columns.
    {
        const auto grid = FitGallery(5, 2000, 200, gap, aspect, caption, narrowest, widest);
        CHECK(grid.Lines(5) == 1 && grid.columns == 5);
        CHECK(grid.width < widest && grid.height <= 200.01f);
    }
    // Too many to show at the narrowest card: that width, as many across as
    // fit, and the rest scrolls.
    {
        const auto grid = FitGallery(92, 640, 300, gap, aspect, caption, narrowest, widest);
        CHECK(grid.width == narrowest);
        CHECK(grid.columns == static_cast<int>((640 + gap) / (narrowest + gap)));
        CHECK(grid.Lines(92) * (grid.height + gap) > 300);
        const auto last = grid.Cell(91, 640, gap);
        CHECK(last.x + grid.width <= 640.01f);
    }
    // Nothing to lay out, or no area: one column, no size.
    {
        const auto none = FitGallery(0, 800, 600, gap, aspect, caption, narrowest, widest);
        CHECK(none.columns == 1 && none.width == 0);
        const auto flat = FitGallery(4, 800, 0, gap, aspect, caption, narrowest, widest);
        CHECK(flat.columns == 1 && flat.width == 0);
    }
    // A single card in a narrow area keeps to the area's width.
    {
        const auto grid = FitGallery(1, 40, 600, gap, aspect, caption, narrowest, widest);
        CHECK(grid.columns == 1 && grid.width == 40);
    }

    // The figure cards are the shape this test lays out above.
    CHECK(FigureCard.aspect == aspect && FigureCard.narrowest == narrowest && FigureCard.widest == widest);
    // The Ultra cards: 16:9 pictures over the name and each command's symbols
    // and words, about 240 px for one command a side (Ryu's Ultra Double) and
    // 380 for two (Gen's). The three of USFIV fill a 1080p body on one line at
    // their widest, stay on one line at 720p, and at 640x720 all still show.
    {
        const float oneCommand = 240, twoCommands = 380;
        const auto full = FitGallery(3, 1840, 880, gap, UltraCard.aspect, twoCommands, UltraCard.narrowest, UltraCard.widest);
        CHECK(full.Lines(3) == 1 && full.width == UltraCard.widest && fits(full, 3, 1840, 880));
        CHECK(3 * UltraCard.widest + 2 * gap <= 1840);
        for (const float inputs : {oneCommand, twoCommands}) {
            const auto small = FitGallery(3, 1200, 520, gap, UltraCard.aspect, inputs, UltraCard.narrowest, UltraCard.widest);
            CHECK(small.Lines(3) == 1 && small.width >= UltraCard.narrowest && fits(small, 3, 1200, 520));
        }
        const auto narrow = FitGallery(3, 600, 560, gap, UltraCard.aspect, oneCommand, UltraCard.narrowest, UltraCard.widest);
        CHECK(narrow.width >= UltraCard.narrowest && fits(narrow, 3, 600, 560));
        // Two Ultras (an earlier edition) are as wide as they go too.
        const auto two = FitGallery(2, 1840, 880, gap, UltraCard.aspect, oneCommand, UltraCard.narrowest, UltraCard.widest);
        CHECK(two.Lines(2) == 1 && two.width == UltraCard.widest);
    }

    // The colour a costume shows and opens on: the one worn where it has it.
    CHECK(GalleryColor({0, 1, 2, 3}, 2) == 2);
    CHECK(GalleryColor({4, 5, 29}, 2) == 4);
    CHECK(GalleryColor({0, 1, 29}, 29) == 29);
    CHECK(GalleryColor({}, 3) == 0);

    std::cout << "Costume and colour grid layout passed.\n";
    return 0;
}
