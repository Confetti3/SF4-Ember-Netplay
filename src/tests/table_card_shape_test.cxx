// How a Classic room's table cards lie on the room board (TableCardShape.hxx):
// one per line they are a versus line at every common screen size, as the
// Classic card was before 1.5.0, and stacked only where the floor is too
// narrow for two players side by side; the Grid keeps its stacked cards. The
// cards fill the column, every card the same height between its least and
// the cap, and only where that does not fit keep their own least, then a
// quiet table one line, then scroll (PlaceTableCards). What a card shows
// steps with its height, and its boxes never overlap (LayOutTableCard).
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
float Height(const MenuRect& r) { return r.y1 - r.y0; }
float Width(const MenuRect& r) { return r.x1 - r.x0; }
const char* Named(TableCardStep step) {
    switch (step) {
    case TableCardStep::Line: return "Line";
    case TableCardStep::Compact: return "Compact";
    case TableCardStep::Medium: return "Medium";
    case TableCardStep::Roomy: break;
    }
    return "Roomy";
}

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

// A card's least (Compact) is its head, its seats at their least and its
// foot, a banner a line more; a quiet table's line is one 44 px line, or its
// head over its seats where too narrow. The stacked card's least is taller.
void TestLeastHeights() {
    namespace card = table_card;
    const auto versus = ShapeTableCards(1150, 2, false, 1);
    const auto stacked = ShapeTableCards(400, 1, false, 1);
    const TableCardContent quiet{true}, seated{}, banner{false, true};
    EXPECT(versus.quietLine && !stacked.quietLine);
    EXPECT(Near(versus.Line(), 44));
    EXPECT(Near(stacked.Line(), card::Head + card::QuietButton + card::Foot));
    EXPECT(Near(versus.Least(seated), card::Head + card::Face + card::Foot));
    EXPECT(Near(versus.Least(seated), 84) && Near(versus.Least(quiet), 84));
    EXPECT(Near(stacked.Least(seated), 112));
    EXPECT(Near(versus.Least(banner) - versus.Least(seated), card::Banner));
    // A quiet line is about half a seated card.
    EXPECT(versus.Line() <= versus.Least(seated) * .55f);
    // At twice the scale a card twice as wide is the same versus line, twice as tall.
    EXPECT(Near(ShapeTableCards(2300, 2, false, 2).Least(seated), versus.Least(seated) * 2));
}

// The step a card's height gives it: by the height of one seat's place, so
// a banner, which takes from the seats, lowers it; a quiet card shorter than
// a whole card is its line. Scaled the same.
void TestStepOfHeight() {
    const auto versus = ShapeTableCards(760, 1, false, 1);
    const auto stacked = ShapeTableCards(400, 1, false, 1);
    const TableCardContent quiet{true}, seated{}, banner{false, true};
    const struct { float height; TableCardStep step; } steps[] = {
        {84, TableCardStep::Compact}, {119, TableCardStep::Compact}, {120, TableCardStep::Medium}, {159, TableCardStep::Medium},
        {160, TableCardStep::Roomy}, {table_card::Cap, TableCardStep::Roomy}};
    for (const auto& want : steps) {
        EXPECT_MSG(versus.StepOf(seated, want.height) == want.step, "height %.0f: %s", want.height, Named(versus.StepOf(seated, want.height)));
        EXPECT_MSG(versus.StepOf(quiet, want.height) == want.step, "quiet height %.0f: %s", want.height, Named(versus.StepOf(quiet, want.height)));
        const auto scaled = ShapeTableCards(760 * 1.5f, 1, false, 1.5f);
        EXPECT_MSG(scaled.StepOf(seated, want.height * 1.5f) == want.step, "height %.0f at 1.5x", want.height);
    }
    EXPECT(versus.StepOf(quiet, 44) == TableCardStep::Line && versus.StepOf(quiet, 83) == TableCardStep::Line);
    EXPECT(stacked.StepOf(quiet, stacked.Line()) == TableCardStep::Line);
    EXPECT(versus.StepOf(banner, 120) == TableCardStep::Compact && versus.StepOf(banner, 138) == TableCardStep::Medium);
    // A stacked card's seats share its body: a Cap card's rows are Medium.
    EXPECT(stacked.StepOf(seated, 112) == TableCardStep::Compact && stacked.StepOf(seated, table_card::Cap) == TableCardStep::Medium);
    // The words grow with the step: the score's size, the fighter's, the seat buttons', the lines under the fighter.
    for (const auto pair : {std::make_pair(TableCardStep::Compact, TableCardStep::Medium), std::make_pair(TableCardStep::Medium, TableCardStep::Roomy)}) {
        const auto low = TypeOf(pair.first), high = TypeOf(pair.second);
        EXPECT(low.score < high.score && low.fighter < high.fighter && low.offer <= high.offer && low.notes < high.notes);
    }
    EXPECT(Near(TypeOf(TableCardStep::Roomy).score, 22) && TypeOf(TableCardStep::Roomy).largeName && !TypeOf(TableCardStep::Medium).largeName);
}

// The fill rule. The cases are the board's own: 780 px is the column of a
// 1950x1070 (logical) window, 375 that of 1280x720 under a seated player's
// two-line bar.
void TestFill() {
    namespace card = table_card;
    const auto line = ShapeTableCards(760, 2, false, 1);
    const TableCardContent quiet{true}, seated{}, banner{false, true};
    const auto equal = [](const TableCardPlaces& places) {
        for (const float h : places.heights) if (!Near(h, places.heights.front())) return false;
        return true;
    };
    // Four tables on a tall column: four equal cards that fill it to within a gap.
    {
        const auto places = PlaceTableCards(line, {seated, quiet, quiet, quiet}, 780);
        EXPECT(places.heights.size() == 4 && equal(places) && !places.scrolls);
        EXPECT_MSG(places.height <= 780 && places.height >= 780 - card::Between, "height %.1f", places.height);
        EXPECT(line.StepOf(seated, places.heights[0]) == TableCardStep::Roomy);
        EXPECT(Near(places.tops[3], 3 * (places.heights[0] + card::Between)));
    }
    // Four at 1280x720: near the compact floor, no scroll, whatever each holds.
    for (const auto& cards : {std::vector<TableCardContent>{seated, seated, seated, seated}, std::vector<TableCardContent>{seated, quiet, quiet, quiet},
             std::vector<TableCardContent>{quiet, quiet, quiet, quiet}}) {
        const auto places = PlaceTableCards(line, cards, 375);
        EXPECT(equal(places) && !places.scrolls && places.height <= 375);
        EXPECT_MSG(places.heights[0] >= 84 && places.heights[0] < 84 + card::Between, "height %.1f", places.heights[0]);
    }
    // A banner raises everyone's least: where four at that least do not fit,
    // each keeps its own (the banner's card 18 px taller), as before the fill.
    {
        const auto places = PlaceTableCards(line, {banner, seated, seated, seated}, 375);
        EXPECT(!places.scrolls && Near(places.heights[0], 102) && Near(places.heights[1], 84) && Near(places.height, 102 + 3 * 84 + 3 * card::Between));
    }
    // Where even the least does not fit, a quiet table is its line and the
    // others keep their least: eight quiet tables at 375 are lines (they
    // still take 394, so the list scrolls by a little).
    {
        const auto places = PlaceTableCards(line, std::vector<TableCardContent>(8, quiet), 375);
        for (const float h : places.heights) EXPECT_MSG(Near(h, card::Quiet), "height %.1f", h);
        EXPECT(Near(places.height, 8 * card::Quiet + 7 * card::Between) && places.scrolls == (places.height > 375));
        const auto mixed = PlaceTableCards(line, {seated, quiet, quiet, quiet, quiet, quiet, quiet, quiet}, 375);
        EXPECT(Near(mixed.heights[0], 84) && Near(mixed.heights[1], card::Quiet));
    }
    // Eight games at 300 scroll, at their least.
    {
        const auto places = PlaceTableCards(line, std::vector<TableCardContent>(8, seated), 300);
        EXPECT(places.scrolls && Near(places.heights[7], 84) && Near(places.height, 8 * 84 + 7 * card::Between));
    }
    // Two tables on a tall column stop at the cap; what is left stays under them.
    {
        const auto places = PlaceTableCards(line, {seated, quiet}, 780);
        EXPECT(Near(places.heights[0], card::Cap) && Near(places.heights[1], card::Cap) && Near(places.tops[1], card::Cap + card::Between));
        EXPECT(Near(places.height, 2 * card::Cap + card::Between) && !places.scrolls);
    }
    // The Grid shares the height among its rows; a row is one height.
    {
        const auto grid = ShapeTableCards(1150, 2, true, 1);
        const auto places = PlaceTableCards(grid, {seated, quiet, quiet, quiet}, 375);
        EXPECT(equal(places) && Near(places.tops[1], 0) && Near(places.tops[2], places.heights[0] + card::Between));
        EXPECT(places.height <= 375 && places.height >= 375 - card::Between);
        // Too short for two rows of stacked cards: a quiet row is its line, the other row its tallest's least.
        const auto lines = PlaceTableCards(grid, {seated, quiet, quiet, quiet}, 160);
        EXPECT(Near(lines.heights[1], lines.heights[0]) && Near(lines.heights[0], 112) && Near(lines.heights[2], grid.Line()));
    }
    // Whatever the column, the least: its tallest row and its whole height,
    // which the board keeps for one row (a watched game's picture over it).
    {
        const auto tall = PlaceTableCards(line, {seated, quiet, banner, quiet}, 1e6f), short_ = PlaceTableCards(line, {seated, quiet, banner, quiet}, 100);
        EXPECT(Near(tall.leastRow, 102) && Near(short_.leastRow, 102) && Near(tall.leastHeight, short_.leastHeight));
        EXPECT(Near(tall.leastHeight, 84 + 2 * card::Quiet + 102 + 3 * card::Between));
    }
    // Whole pixels, so the cards never overrun the column (no scroll bar for a hair).
    for (const float area : {500.5f, 600.3f, 790.9f}) {
        const auto places = PlaceTableCards(ShapeTableCards(760 * 1.218f, 1, false, 1.218f), std::vector<TableCardContent>(4, seated), area);
        EXPECT_MSG(places.height <= area && !places.scrolls && Near(places.heights[0], std::floor(places.heights[0])), "area %.1f", area);
    }
    EXPECT(PlaceTableCards(line, {}, 375).heights.empty() && Near(PlaceTableCards(line, {}, 375).height, 0));
}

// A card's boxes at every step: its line, seats, pictures, score, buttons,
// words and banner lie inside the card and never over one another, its
// buttons never over its lead (TABLE n and its state), a picture inside its
// seat's place (never taller than the body), on every shape and however many
// buttons want room.
void TestLayOutNeverOverlaps() {
    namespace card = table_card;
    for (const float width : {760.f, 1150.f, 560.f, 300.f})
        for (const float height : {84.f, 102.f, 120.f, 160.f, card::Cap})
            for (const bool banner : {false, true})
                for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{2}})
                    for (const float want : {80.f, 400.f}) {
                        const auto shape = ShapeTableCards(width, 1, false, 1);
                        TableCardInput in;
                        in.content = {false, banner};
                        in.card = {0, 0, width, (std::max)(height, shape.Least(in.content))};
                        in.lead = 130; in.words = 120; in.score = 40;
                        in.buttons.assign(count, want);
                        const auto at = LayOutTableCard(shape, in);
                        const char* step = Named(at.step);
                        const MenuRect inner{card::Pad, 0, width - card::Pad, in.card.y1};
                        const MenuRect body{card::Pad, at.head.y1, width - card::Pad, in.card.y1 - (card::Foot + (banner ? card::Banner : 0))};
                        EXPECT_MSG(at.step == shape.StepOf(in.content, in.card.y1), "width %.0f height %.0f: the layout's step is not its height's", width, height);
                        EXPECT_MSG(Inside(at.head, inner) && Inside(at.seats[0], body) && Inside(at.seats[1], body) && (!banner || Inside(at.banner, inner)),
                            "width %.0f height %.0f %s buttons %zu want %.0f: a box left the card", width, height, step, count, want);
                        EXPECT_MSG(Apart(at.seats[0], at.seats[1]) && Apart(at.head, at.seats[0]) && Apart(at.head, at.seats[1]) && Apart(at.seats[1], at.banner),
                            "width %.0f height %.0f %s: the seats, line and banner overlap", width, height, step);
                        for (std::size_t side = 0; side < 2; ++side) {
                            const auto& face = at.faces[side];
                            EXPECT_MSG(Inside(face, at.seats[side]) && Height(face) <= Height(body) + .01f && Near(Width(face), Height(face)),
                                "width %.0f height %.0f %s: a picture left its seat or the body", width, height, step);
                            EXPECT_MSG(Apart(face, at.score), "width %.0f height %.0f %s: a picture stood over the score", width, height, step);
                        }
                        // The pictures grow with the card: as tall as the body allows on a versus card wide enough.
                        if (shape.versus && Width(at.seats[0]) - (card::FaceGap + card::SeatTextLeast) >= Height(body))
                            EXPECT_MSG(Near(Height(at.faces[0]), Height(body)), "width %.0f height %.0f %s: a picture short of the body", width, height, step);
                        EXPECT_MSG(Height(at.faces[0]) >= (std::min)(Height(body), shape.versus ? card::Face : 0.f) - .01f,
                            "width %.0f height %.0f %s: a picture under its least", width, height, step);
                        EXPECT_MSG(at.buttons.size() == count, "width %.0f: buttons lost", width);
                        for (std::size_t i = 0; i < count; ++i) {
                            EXPECT_MSG(Inside(at.buttons[i], at.head) && at.buttons[i].x0 >= card::Pad + in.lead - .01f,
                                "width %.0f buttons %zu want %.0f: a button left its line or stood over its lead", width, count, want);
                            // Pressed on the line's whole height (drawn inside it).
                            EXPECT_MSG(Near(at.buttons[i].y0, at.head.y0) && Near(at.buttons[i].y1, at.head.y1), "width %.0f: a button is not as tall as its line", width);
                            if (i) EXPECT_MSG(Apart(at.buttons[i - 1], at.buttons[i]), "width %.0f: buttons overlap", width);
                            EXPECT_MSG(Apart(at.buttons[i], at.words) && Apart(at.buttons[i], at.score), "width %.0f: a button stood over the words or the score", width);
                        }
                        if (!shape.versus) EXPECT_MSG(Apart(at.score, at.words), "width %.0f: the score stood over the words", width);
                        else EXPECT_MSG(Apart(at.score, at.seats[0]) && Apart(at.score, at.seats[1]), "width %.0f: the score stood over a seat", width);
                    }
    // A quiet line: its seats are its offers, beside its lead and words on one
    // line, or under them on a narrow card; words that want more than the line
    // has give way before either seat goes under its least.
    for (const float width : {760.f, 300.f})
        for (const float words : {200.f, 600.f}) {
            const auto shape = ShapeTableCards(width, 1, false, 1);
            TableCardInput in;
            in.card = {0, 0, width, shape.Line()};
            in.content = {true};
            in.lead = 130; in.words = words;
            const auto at = LayOutTableCard(shape, in);
            EXPECT(at.step == TableCardStep::Line && at.buttons.empty());
            EXPECT(Apart(at.seats[0], at.seats[1]) && Apart(at.words, at.seats[0]) && Apart(at.head, at.seats[0]));
            EXPECT_MSG(Width(at.seats[0]) >= (shape.quietLine ? card::OfferLeast : 0) - .01f &&
                Width(at.seats[1]) >= (shape.quietLine ? card::OfferLeast : 0) - .01f, "width %.0f words %.0f: a seat under its least", width, words);
            EXPECT(Inside(at.seats[1], {0, 0, width, in.card.y1}));
        }
    // A quiet card as tall as a whole card: its head, then its two seats, each
    // half the body, as tall as the body, side by side.
    for (const float width : {760.f, 300.f})
        for (const float height : {112.f, 140.f, card::Cap}) {
            const auto shape = ShapeTableCards(width, 1, false, 1);
            TableCardInput in;
            in.card = {0, 0, width, height};
            in.content = {true};
            in.lead = 130; in.words = 60;
            const auto at = LayOutTableCard(shape, in);
            const float body = height - card::Head - card::Foot;
            EXPECT_MSG(at.step != TableCardStep::Line && Near(Height(at.seats[0]), body) && Near(Height(at.seats[1]), body) &&
                Near(at.seats[0].y0, card::Head) && at.seats[0].x1 <= at.seats[1].x0 && Near(Width(at.seats[0]), Width(at.seats[1])) &&
                Near(at.seats[0].x0, card::Pad) && Near(at.seats[1].x1, width - card::Pad),
                "width %.0f height %.0f: a quiet card's seats are not its body's two halves", width, height);
            EXPECT(Apart(at.head, at.seats[0]) && Apart(at.words, at.seats[0]) && Width(at.score) <= 0);
        }
    // A question on a quiet card (a watcher's seat that would end the view):
    // two or three answers share the seats' place, apart, inside the card and
    // clear of its words, the seats standing aside; on its line, on two, and
    // on a tall card across its body, as tall as the seats were.
    for (const float width : {760.f, 300.f})
        for (const bool tall : {false, true})
            for (const std::size_t count : {std::size_t{2}, std::size_t{3}}) {
                const auto shape = ShapeTableCards(width, 1, false, 1);
                TableCardInput in;
                in.card = {0, 0, width, tall ? card::Cap : shape.Line()};
                in.content = {true};
                in.lead = 130; in.words = 60;
                const auto seated = LayOutTableCard(shape, in);
                in.buttons.assign(count, 90);
                const auto at = LayOutTableCard(shape, in);
                const MenuRect span{seated.seats[0].x0, seated.seats[0].y0, seated.seats[1].x1, seated.seats[1].y1};
                EXPECT_MSG(at.buttons.size() == count, "width %.0f answers %zu: answers lost", width, count);
                EXPECT(tall ? at.step != TableCardStep::Line : at.step == TableCardStep::Line);
                for (std::size_t i = 0; i < count; ++i) {
                    EXPECT_MSG(Inside(at.buttons[i], span) && Apart(at.buttons[i], at.words) && Inside(at.buttons[i], {card::Pad, 0, width - card::Pad, in.card.y1}),
                        "width %.0f answers %zu: an answer left the seats' place or stood over the words", width, count);
                    if (i) EXPECT_MSG(Apart(at.buttons[i - 1], at.buttons[i]), "width %.0f answers %zu: answers overlap", width, count);
                    if (tall) EXPECT_MSG(Near(Height(at.buttons[i]), Height(span)), "width %.0f answers %zu: a tall card's answer not as tall as its seats", width, count);
                }
                // Across the place: the first answer at its start, the last at its end.
                EXPECT(Near(at.buttons.front().x0, span.x0) && Near(at.buttons.back().x1, span.x1));
                EXPECT(Near(Width(at.seats[0]), 0) && Near(Width(at.seats[1]), 0));
            }
}
}

int main() {
    TestOnePerLineIsAVersusLineAtCommonSizes();
    TestVersusStartsWhereEachSeatHasItsRoom();
    TestGridKeepsItsColumns();
    TestLeastHeights();
    TestStepOfHeight();
    TestFill();
    TestLayOutNeverOverlaps();
    if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
    else std::printf("TableCardShape tests passed\n");
    return TEST_EXIT_CODE();
}
