// How a Classic room's table cards lie on the room board (TableCardShape.hxx):
// one per line they are a versus line at every common screen size, as the
// Classic card was before 1.5.0, and stacked only where the floor is too
// narrow for two players side by side; the Grid keeps its stacked cards. Each
// card is as tall as what it holds, a quiet table one line.
#include "../ui/TableCardShape.hxx"
#include "test_support.hxx"

#include <cmath>
#include <initializer_list>
#include <cstdio>

using namespace sf4e::ui;

namespace {
bool Near(float a, float b) { return std::fabs(a - b) < .01f; }

// One per line: the floor of a 1280-wide board (about 760 logical pixels),
// of a 1920 one, and of 1280x720 at 150% in the narrow tabs (about 820), is a
// versus line; 640x720 at 150% (about 400) stacks the seats.
void TestOnePerLineIsAVersusLineAtCommonSizes() {
    for (const float width : {700.f, 760.f, 820.f, 1150.f, 2600.f}) {
        const auto shape = ShapeTableCards(width, 2, false, 1);
        EXPECT_MSG(shape.columns == 1 && shape.versus, "width %.0f", width);
    }
    EXPECT(!ShapeTableCards(400, 1, false, 1).versus);
    // Scaled: the same logical floor gives the same shape.
    EXPECT(ShapeTableCards(760 * 1.5f, 2, false, 1.5f).versus);
    EXPECT(!ShapeTableCards(400 * 1.5f, 1, false, 1.5f).versus);
}

// The versus line starts where each player's lines get their least room.
void TestVersusStartsWhereEachSeatHasItsRoom() {
    namespace card = table_card;
    const float least = card::VersusLeast;
    EXPECT(ShapeTableCards(least, 1, false, 1).versus);
    EXPECT(!ShapeTableCards(least - 1, 1, false, 1).versus);
    const float seatText = (least - 2 * card::Pad - card::Middle) * .5f - card::Face - card::FaceGap;
    EXPECT(Near(seatText, card::SeatTextLeast));
}

// The Grid keeps two or three cards across, stacked where each card is
// narrow (a 1920 board's two across), a versus line only where each card is
// wide enough (three across on a 32:9 screen).
void TestGridKeepsItsColumns() {
    const auto two = ShapeTableCards(1150, 2, true, 1);
    EXPECT(two.columns == 2 && !two.versus);
    const auto three = ShapeTableCards(2400, 3, true, 1);
    EXPECT(three.columns == 3 && three.versus);
    EXPECT(ShapeTableCards(500, 0, true, 1).columns == 1);
}

// A card is as tall as what it holds: a quiet table one line (two where the
// card is too narrow for its seats beside its head), anyone at it the head,
// the seats and the foot, a banner and your own place's strip each a line
// more. The stacked card stays taller than the versus line.
void TestHeights() {
    namespace card = table_card;
    const auto versus = ShapeTableCards(1150, 2, false, 1);
    const auto stacked = ShapeTableCards(400, 1, false, 1);
    const TableCardContent quiet{true}, seated{}, banner{false, true}, own{false, false, true};
    EXPECT(versus.quietLine && !stacked.quietLine);
    EXPECT(Near(versus.Height(quiet), card::Quiet));
    EXPECT(Near(stacked.Height(quiet), card::Head + card::QuietButton + card::Foot));
    EXPECT(Near(versus.Height(seated), card::Head + card::Face + card::Foot));
    EXPECT(Near(versus.Height(seated), 96));
    EXPECT(Near(stacked.Height(seated), 124));
    EXPECT(versus.Height(seated) < stacked.Height(seated));
    EXPECT(Near(versus.Height(banner) - versus.Height(seated), card::Banner));
    EXPECT(Near(versus.Height(own) - versus.Height(seated), card::Strip));
    // A quiet card is about half a seated one.
    EXPECT(versus.Height(quiet) * 2 <= versus.Height(seated));
    // At twice the scale a card twice as wide is the same versus line, twice as tall.
    EXPECT(Near(ShapeTableCards(2300, 2, false, 2).Height(seated), versus.Height(seated) * 2));
}

// Cards stand one under another, a row of a grid one height (its tallest),
// and the floor is as tall as its rows: four quiet tables and a game fit
// where four games would not.
void TestPlaces() {
    namespace card = table_card;
    const auto line = ShapeTableCards(760, 2, false, 1);
    const TableCardContent quiet{true}, seated{};
    const auto places = PlaceTableCards(line, {seated, quiet, quiet, quiet});
    EXPECT(places.tops.size() == 4 && Near(places.tops[1], 96 + card::Between) && Near(places.heights[1], card::Quiet));
    EXPECT(Near(places.height, 96 + 3 * card::Quiet + 3 * card::Between) && Near(places.tallest, 96));
    const auto grid = PlaceTableCards(ShapeTableCards(1150, 2, true, 1), {seated, quiet, quiet, quiet});
    EXPECT(Near(grid.tops[1], 0) && Near(grid.heights[1], grid.heights[0]) && grid.heights[2] < grid.heights[0]);
    EXPECT(Near(grid.height, grid.heights[0] + card::Between + grid.heights[2]));
    EXPECT(PlaceTableCards(line, {}).height == 0);
}
}

int main() {
    TestOnePerLineIsAVersusLineAtCommonSizes();
    TestVersusStartsWhereEachSeatHasItsRoom();
    TestGridKeepsItsColumns();
    TestHeights();
    TestPlaces();
    if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
    else std::printf("TableCardShape tests passed\n");
    return TEST_EXIT_CODE();
}
