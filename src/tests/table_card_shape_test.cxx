// How a Classic room's table cards lie on the room board (TableCardShape.hxx):
// one per line they are a versus line at every common screen size, as the
// Classic card was before 1.5.0, and stacked only where the floor is too
// narrow for two players side by side; the Grid keeps its stacked cards. Each
// card is as tall as what it holds, a quiet table one line, and its boxes
// never overlap (LayOutTableCard).
#include "../ui/TableCardShape.hxx"
#include "test_support.hxx"

#include <cmath>
#include <initializer_list>
#include <cstdio>

using namespace sf4e::ui;

namespace {
bool Near(float a, float b) { return std::fabs(a - b) < .01f; }
bool Inside(const MenuRect& inner, const MenuRect& outer) {
    return inner.x0 >= outer.x0 - .01f && inner.x1 <= outer.x1 + .01f && inner.y0 >= outer.y0 - .01f && inner.y1 <= outer.y1 + .01f;
}
bool Apart(const MenuRect& a, const MenuRect& b) { return a.x1 <= b.x0 + .01f || b.x1 <= a.x0 + .01f || a.y1 <= b.y0 + .01f || b.y1 <= a.y0 + .01f; }

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
// the seats and the foot, a banner a line more. Your own place has no strip:
// what leaves it stands on its line. The stacked card stays taller.
void TestHeights() {
    namespace card = table_card;
    const auto versus = ShapeTableCards(1150, 2, false, 1);
    const auto stacked = ShapeTableCards(400, 1, false, 1);
    const TableCardContent quiet{true}, seated{}, banner{false, true};
    EXPECT(versus.quietLine && !stacked.quietLine);
    EXPECT(Near(versus.Height(quiet), 44));
    EXPECT(Near(stacked.Height(quiet), card::Head + card::QuietButton + card::Foot));
    EXPECT(Near(versus.Height(seated), card::Head + card::Face + card::Foot));
    EXPECT(Near(versus.Height(seated), 84));
    EXPECT(Near(stacked.Height(seated), 112));
    EXPECT(versus.Height(seated) < stacked.Height(seated));
    EXPECT(Near(versus.Height(banner) - versus.Height(seated), card::Banner));
    // A quiet card is about half a seated one.
    EXPECT(versus.Height(quiet) <= versus.Height(seated) * .55f);
    // At twice the scale a card twice as wide is the same versus line, twice as tall.
    EXPECT(Near(ShapeTableCards(2300, 2, false, 2).Height(seated), versus.Height(seated) * 2));
}

// Cards stand one under another, a row of a grid one height (its tallest),
// and the floor is as tall as its rows. Four games, the busiest four tables,
// take 4 x 84 + 3 x 6 = 354 px: they fit the 375 the column has at 1280x720
// under a two-line bar.
void TestPlaces() {
    namespace card = table_card;
    const auto line = ShapeTableCards(760, 2, false, 1);
    const TableCardContent quiet{true}, seated{}, banner{false, true};
    const auto places = PlaceTableCards(line, {seated, quiet, quiet, quiet});
    EXPECT(places.tops.size() == 4 && Near(places.tops[1], 84 + card::Between) && Near(places.heights[1], card::Quiet));
    EXPECT(Near(places.height, 84 + 3 * card::Quiet + 3 * card::Between) && Near(places.tallest, 84));
    EXPECT(Near(PlaceTableCards(line, {seated, seated, seated, seated}).height, 354));
    EXPECT(PlaceTableCards(line, {seated, seated, seated, banner}).height <= 375);
    const auto grid = PlaceTableCards(ShapeTableCards(1150, 2, true, 1), {seated, quiet, quiet, quiet});
    EXPECT(Near(grid.tops[1], 0) && Near(grid.heights[1], grid.heights[0]) && grid.heights[2] < grid.heights[0]);
    EXPECT(Near(grid.height, grid.heights[0] + card::Between + grid.heights[2]));
    EXPECT(PlaceTableCards(line, {}).height == 0);
}

// A card's boxes: its line, seats, score, buttons, words and banner lie inside
// the card and never over one another, its buttons never over its lead (TABLE n
// and its state), on every shape and however many buttons want room.
void TestLayOutNeverOverlaps() {
    namespace card = table_card;
    for (const float width : {760.f, 560.f, 300.f})
        for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{2}})
            for (const float want : {80.f, 400.f}) {
                const auto shape = ShapeTableCards(width, 1, false, 1);
                TableCardInput in;
                in.card = {0, 0, width, shape.Height({false, true})};
                in.content = {false, true};
                in.lead = 130; in.words = 120; in.score = 40;
                in.buttons.assign(count, want);
                const auto at = LayOutTableCard(shape, in);
                const MenuRect inner{card::Pad, 0, width - card::Pad, in.card.y1};
                EXPECT_MSG(Inside(at.head, inner) && Inside(at.seats[0], inner) && Inside(at.seats[1], inner) && Inside(at.banner, inner),
                    "width %.0f buttons %zu want %.0f: a box left the card", width, count, want);
                EXPECT_MSG(Apart(at.seats[0], at.seats[1]) && Apart(at.head, at.seats[0]) && Apart(at.seats[1], at.banner),
                    "width %.0f: the seats, line and banner overlap", width);
                EXPECT_MSG(at.buttons.size() == count, "width %.0f: buttons lost", width);
                for (std::size_t i = 0; i < count; ++i) {
                    EXPECT_MSG(Inside(at.buttons[i], at.head) && at.buttons[i].x0 >= card::Pad + in.lead - .01f,
                        "width %.0f buttons %zu want %.0f: a button left its line or stood over its lead", width, count, want);
                    if (i) EXPECT_MSG(Apart(at.buttons[i - 1], at.buttons[i]), "width %.0f: buttons overlap", width);
                    EXPECT_MSG(Apart(at.buttons[i], at.words) && Apart(at.buttons[i], at.score), "width %.0f: a button stood over the words or the score", width);
                }
                if (!shape.versus) EXPECT_MSG(Apart(at.score, at.words), "width %.0f: the score stood over the words", width);
            }
    // A quiet card has no line buttons: its seats are its offers, beside its
    // lead and words on one line, or under them on a narrow card.
    for (const float width : {760.f, 300.f}) {
        const auto shape = ShapeTableCards(width, 1, false, 1);
        TableCardInput in;
        in.card = {0, 0, width, shape.Height({true})};
        in.content = {true};
        in.lead = 130; in.words = 200;
        in.buttons = {90, 90};
        const auto at = LayOutTableCard(shape, in);
        EXPECT(at.buttons.empty());
        EXPECT(Apart(at.seats[0], at.seats[1]) && Apart(at.words, at.seats[0]) && Apart(at.head, at.seats[0]));
        EXPECT(at.seats[0].x1 - at.seats[0].x0 >= (shape.quietLine ? card::OfferLeast : 0) - .01f);
        EXPECT(Inside(at.seats[1], {0, 0, width, in.card.y1}));
    }
}
}

int main() {
    TestOnePerLineIsAVersusLineAtCommonSizes();
    TestVersusStartsWhereEachSeatHasItsRoom();
    TestGridKeepsItsColumns();
    TestHeights();
    TestPlaces();
    TestLayOutNeverOverlaps();
    if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
    else std::printf("TableCardShape tests passed\n");
    return TEST_EXIT_CODE();
}
