// How a Classic room's table cards lie on the room board (TableCardShape.hxx):
// one per line they are a versus line at every common screen size, as the
// Classic card was before 1.5.0, and stacked only where the floor is too
// narrow for two players side by side; the Grid keeps its stacked cards.
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
        const auto shape = ShapeTableCards(width, 2, false, false, 1);
        EXPECT_MSG(shape.columns == 1 && shape.versus, "width %.0f", width);
    }
    EXPECT(!ShapeTableCards(400, 1, false, false, 1).versus);
    // Scaled: the same logical floor gives the same shape.
    EXPECT(ShapeTableCards(760 * 1.5f, 2, false, false, 1.5f).versus);
    EXPECT(!ShapeTableCards(400 * 1.5f, 1, false, false, 1.5f).versus);
}

// The versus line starts where each player's lines get their least room.
void TestVersusStartsWhereEachSeatHasItsRoom() {
    namespace card = table_card;
    const float least = card::VersusLeast;
    EXPECT(ShapeTableCards(least, 1, false, false, 1).versus);
    EXPECT(!ShapeTableCards(least - 1, 1, false, false, 1).versus);
    const float seatText = (least - 2 * card::Pad - card::Middle) * .5f - card::Face - card::FaceGap;
    EXPECT(Near(seatText, card::SeatTextLeast));
}

// The Grid keeps two or three cards across, stacked where each card is
// narrow (a 1920 board's two across), a versus line only where each card is
// wide enough (three across on a 32:9 screen).
void TestGridKeepsItsColumns() {
    const auto two = ShapeTableCards(1150, 2, true, false, 1);
    EXPECT(two.columns == 2 && !two.versus);
    const auto three = ShapeTableCards(2400, 3, true, false, 1);
    EXPECT(three.columns == 3 && three.versus);
    EXPECT(ShapeTableCards(500, 0, true, false, 1).columns == 1);
}

// A card holds its line, its seats and its foot; a banner adds its line to
// every card, so cards in a row stay one height. The stacked card keeps the
// height it had; the versus card is shorter, its two seats side by side.
void TestHeights() {
    namespace card = table_card;
    const auto versus = ShapeTableCards(1150, 2, false, false, 1);
    const auto stacked = ShapeTableCards(400, 1, false, false, 1);
    EXPECT(Near(versus.height, card::Header + card::Face + card::Gap + card::Footer));
    EXPECT(Near(stacked.height, 182));
    EXPECT(versus.height < stacked.height);
    EXPECT(Near(ShapeTableCards(1150, 2, false, true, 1).height - versus.height, card::Banner));
    EXPECT(Near(ShapeTableCards(400, 1, false, true, 1).height, 200));
    EXPECT(Near(ShapeTableCards(1150, 2, false, false, 2).height, versus.height * 2));
}
}

int main() {
    TestOnePerLineIsAVersusLineAtCommonSizes();
    TestVersusStartsWhereEachSeatHasItsRoom();
    TestGridKeepsItsColumns();
    TestHeights();
    if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
    else std::printf("TableCardShape tests passed\n");
    return TEST_EXIT_CODE();
}
