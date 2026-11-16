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

// A stacked card's picture leaves its player's lines the same 200 px a versus
// card does (FaceSize), shrinking to a Compact picture (48 px, or the row where
// that is shorter) where even that does not leave them.
void TestStackedCardKeepsItsText() {
    namespace card = table_card;
    for (const float s : {1.f, 1.5f})
        for (const float logical : {260.f, 290.f, 330.f, 420.f, 600.f}) {
            const auto shape = ShapeTableCards(logical * s, 1, false, s);
            EXPECT_MSG(!shape.versus, "width %.0f scale %.1f is a versus card", logical, s);
            for (const float height : {112.f, 140.f, card::Cap}) {
                TableCardInput in;
                in.content = {false, false};
                in.card = {0, 0, logical * s, (std::max)(height * s, shape.Least(in.content))};
                in.lead = 130 * s; in.words = 60 * s; in.score = 40 * s;
                const auto at = LayOutTableCard(shape, in);
                for (std::size_t side = 0; side < 2; ++side) {
                    const float seat = Width(at.seats[side]), text = seat - Width(at.faces[side]) - card::FaceGap * s;
                    if (seat >= (card::Face + card::FaceGap + card::SeatTextLeast) * s - .01f)
                        EXPECT_MSG(text >= card::SeatTextLeast * s - .01f, "width %.0f scale %.1f height %.0f: a stacked card's text %.1f is under its least", logical, s, height, text);
                    else
                        EXPECT_MSG(Near(Width(at.faces[side]), (std::min)(card::Face * s, Height(at.seats[side]))), "width %.0f scale %.1f height %.0f: a stacked picture is not a Compact one", logical, s, height);
                    EXPECT_MSG(Inside(at.faces[side], at.seats[side]) && Width(at.faces[side]) <= seat * .5f + .01f, "width %.0f scale %.1f: a stacked picture left its row or passed half the card", logical, s);
                }
            }
        }
}

// A Compact seat's tag line: the note beside the name (a streak, else the idle
// time) lies inside the seat's lines, clear of the name, on its inner side, the
// pair centred on a versus card; where both do not fit the name stands alone.
void TestSeatTagPlacesItsNote() {
    namespace card = table_card;
    for (const float s : {1.f, 1.5f})
        for (const bool versus : {false, true})
            for (const bool mirrored : {false, true}) {
                const float left = 100 * s, room = 200 * s, tag = 90 * s, note = 60 * s;
                const MenuRect seat{left, 0, left + room, 1};
                const auto at = PlaceSeatTag(left, room, tag, note, versus, mirrored, s);
                const MenuRect name{at.tag, 0, at.tag + tag, 1}, beside{at.note, 0, at.note + note, 1};
                EXPECT_MSG(at.noted && Inside(name, seat) && Inside(beside, seat) && Apart(name, beside), "scale %.1f versus %d mirrored %d: the idle time did not stand in the seat beside the name", s, versus, mirrored);
                EXPECT_MSG(mirrored ? beside.x1 <= name.x0 - card::Space * s + .01f : beside.x0 >= name.x1 + card::Space * s - .01f, "scale %.1f mirrored %d: the note is not on the name's inner side", s, mirrored);
                const float pairLeft = (std::min)(name.x0, beside.x0), pairRight = (std::max)(name.x1, beside.x1);
                if (versus) EXPECT_MSG(Near(pairLeft - left, left + room - pairRight), "scale %.1f mirrored %d: the pair is not centred", s, mirrored);
                else EXPECT_MSG(Near(pairLeft, left), "scale %.1f mirrored %d: a stacked seat's pair does not start at its picture", s, mirrored);
                // Too wide for both: the name alone, inside the seat; no note: the name alone.
                const auto tight = PlaceSeatTag(left, room, 150 * s, note, versus, mirrored, s);
                EXPECT_MSG(!tight.noted && Inside({tight.tag, 0, tight.tag + 150 * s, 1}, seat), "scale %.1f: a note that does not fit was placed", s);
                EXPECT(!PlaceSeatTag(left, room, tag, 0, versus, mirrored, s).noted);
            }
}

// A card's line holds two buttons (another table's Join queue and Watch) beside
// its state at the common sizes (1280x720 at scale 1 and 1.5: a versus card, and
// a stacked one beside the score), apart, inside the head, as wide as they want.
// Where the two do not fit, the card's name gives way first and then the buttons
// share what is left, their labels cut, never over TABLE n and its state. (Your
// own card's buttons are its strip's: TestOwnCardStrip.)
void TestCardLineHoldsTwoButtons() {
    namespace card = table_card;
    for (const float s : {1.f, 1.5f})
        for (const float logical : {760.f, 400.f, 300.f}) {
            const auto shape = ShapeTableCards(logical * s, 1, false, s);
            TableCardInput in;
            in.content = {false, false};
            in.card = {0, 0, logical * s, (std::max)(card::Cap * s * .5f, shape.Least(in.content))};
            in.lead = 130 * s; in.words = 120 * s; in.score = 40 * s;
            const float ready = 76 * s, leave = 84 * s;
            in.buttons = {ready, leave};
            const auto at = LayOutTableCard(shape, in);
            EXPECT_MSG(at.buttons.size() == 2, "width %.0f scale %.1f: a button was lost", logical, s);
            const float room = (shape.versus ? at.head.x1 : at.score.x0 - card::Space * s) - (at.head.x0 + in.lead);
            const bool fits = ready + leave + card::Between * s <= room + .01f;
            for (std::size_t i = 0; i < 2; ++i) {
                EXPECT_MSG(Inside(at.buttons[i], at.head) && at.buttons[i].x0 >= at.head.x0 + in.lead - .01f, "width %.0f scale %.1f: a button left the head or stood over TABLE n and its state", logical, s);
                EXPECT_MSG(Apart(at.buttons[i], at.words) && Apart(at.buttons[i], at.score), "width %.0f scale %.1f: a button stood over the name or the score", logical, s);
            }
            EXPECT_MSG(Apart(at.buttons[0], at.buttons[1]) && at.buttons[0].x1 <= at.buttons[1].x0 - card::Between * s + .01f, "width %.0f scale %.1f: the buttons touch", logical, s);
            if (logical >= 400) EXPECT_MSG(fits, "width %.0f scale %.1f: the test's own widths no longer fit", logical, s);
            if (fits) EXPECT_MSG(Near(Width(at.buttons[0]), ready) && Near(Width(at.buttons[1]), leave), "width %.0f scale %.1f: buttons that fit were not as wide as they want", logical, s);
            else EXPECT_MSG(Width(at.buttons[0]) < ready && Width(at.buttons[1]) < leave && Near(Width(at.words), 0), "width %.0f scale %.1f: neither the name nor the buttons gave way", logical, s);
        }
}

// Your own card's strip (Ready, Leave seat, Table options; Leave queue and
// Table options in the queue): its band is part of the card's least height, a
// Between under the banner and a Foot above the card's end; its buttons are
// equal, a Between apart, across the card's inner width, as tall as the strip,
// and the card's line holds none. Nothing else stands over the strip: not the
// seats, their pictures, the score, the banner or the words, at any step, on
// the versus card and the stacked one, scaled.
void TestOwnCardStrip() {
    namespace card = table_card;
    const TableCardContent seated{}, own{false, false, true}, ownBanner{false, true, true};
    // Heights at scale 1 and 1.5: 32 px of buttons and the gap above them.
    for (const float s : {1.f, 1.5f}) {
        const auto versus = ShapeTableCards(760 * s, 1, false, s), stacked = ShapeTableCards(400 * s, 1, false, s);
        EXPECT_MSG(Near(own.Under(), card::Strip + card::Between) && Near(card::Strip, 32), "scale %.1f: the strip's band is not 38 px", s);
        EXPECT_MSG(Near(versus.Least(own) - versus.Least(seated), (card::Strip + card::Between) * s), "scale %.1f: the strip is not part of a versus card's least", s);
        EXPECT_MSG(Near(stacked.Least(own) - stacked.Least(seated), (card::Strip + card::Between) * s), "scale %.1f: the strip is not part of a stacked card's least", s);
        EXPECT_MSG(Near(versus.Least(own), 122 * s) && Near(stacked.Least(own), 150 * s), "scale %.1f: own card's least %.1f / %.1f", s, versus.Least(own), stacked.Least(own));
        EXPECT_MSG(Near(versus.Least(ownBanner) - versus.Least(own), card::Banner * s), "scale %.1f: the banner and the strip do not add", s);
        // The step is by the seats' height under the strip: Compact at the least, Medium from 158, Roomy from 198.
        EXPECT_MSG(versus.StepOf(own, 122 * s) == TableCardStep::Compact && versus.StepOf(own, 157 * s) == TableCardStep::Compact &&
            versus.StepOf(own, 158 * s) == TableCardStep::Medium && versus.StepOf(own, 197 * s) == TableCardStep::Medium &&
            versus.StepOf(own, 198 * s) == TableCardStep::Roomy, "scale %.1f: an own card's step is not by the seats' height under its strip", s);
    }
    // The fill rule with your seat among the tables.
    {
        const auto line = ShapeTableCards(760, 2, false, 1);
        const TableCardContent quiet{true};
        // 1920x1080's column: four equal cards, the strip's card as tall as the rest.
        const auto tall = PlaceTableCards(line, {own, seated, seated, seated}, 780);
        EXPECT(!tall.scrolls && Near(tall.heights[0], tall.heights[3]) && tall.heights[0] >= 122 && tall.height <= 780);
        EXPECT(line.StepOf(own, tall.heights[0]) == TableCardStep::Medium);
        // 1280x720 (375): your card keeps its 122, the quiet tables are lines, no scroll bar.
        for (const auto& cards : {std::vector<TableCardContent>{own, quiet, quiet, quiet}, std::vector<TableCardContent>{quiet, own, quiet, seated},
                 std::vector<TableCardContent>{own, seated, seated, quiet}}) {
            const auto places = PlaceTableCards(line, cards, 375);
            EXPECT_MSG(!places.scrolls && !places.stripAside && places.height <= 375, "720p with your seat: height %.1f scrolls or lost the strip", places.height);
            for (std::size_t i = 0; i < cards.size(); ++i) EXPECT(places.heights[i] >= (cards[i].quiet ? card::Quiet : line.Least(cards[i])) - .01f);
        }
        // Four games with your seat among them need 392 px with the strip: 17 more than that
        // column. The strip stands aside (the buttons on the card's line), and four cards of 89 fit.
        const auto games = PlaceTableCards(line, {own, seated, seated, seated}, 375);
        EXPECT(!games.scrolls && games.stripAside && Near(games.heights[0], 89) && Near(games.heights[3], 89));
        // With a banner on your card too (410 with the strip): its card keeps its 102 and the rest their 84.
        const auto banner = PlaceTableCards(line, {ownBanner, seated, seated, seated}, 375);
        EXPECT(!banner.scrolls && banner.stripAside && Near(banner.heights[0], 102) && Near(banner.heights[1], 84) && Near(banner.height, 102 + 3 * 84 + 3 * card::Between));
        // Where the list scrolls with or without it (eight games), the strip stays.
        const auto many = PlaceTableCards(line, {own, seated, seated, seated, seated, seated, seated, seated}, 300);
        EXPECT(many.scrolls && !many.stripAside && Near(many.heights[0], 122));
        // Nobody's strip, nothing aside.
        EXPECT(!PlaceTableCards(line, {seated, seated, seated, seated}, 300).stripAside);
    }
    // Standing aside: the three buttons are the line's, apart, inside the head, never over its lead.
    for (const float s : {1.f, 1.5f})
        for (const float logical : {760.f, 400.f}) {
            const auto shape = ShapeTableCards(logical * s, 1, false, s);
            TableCardInput in;
            in.card = {0, 0, logical * s, shape.Least(in.content)};
            in.lead = 130 * s; in.words = 120 * s; in.score = 40 * s;
            in.buttons = {70 * s, 80 * s, 100 * s};
            const auto at = LayOutTableCard(shape, in);
            EXPECT_MSG(at.buttons.size() == 3 && Width(at.strip) <= 0, "scale %.1f width %.0f: a card with no strip lost a button or kept a strip", s, logical);
            for (std::size_t i = 0; i < at.buttons.size(); ++i) {
                EXPECT_MSG(Inside(at.buttons[i], at.head) && at.buttons[i].x0 >= at.head.x0 + in.lead - .01f, "scale %.1f width %.0f: a line button left the head or stood over its lead", s, logical);
                if (i) EXPECT_MSG(Apart(at.buttons[i - 1], at.buttons[i]), "scale %.1f width %.0f: line buttons overlap", s, logical);
            }
        }
    // Where the boxes stand.
    for (const float s : {1.f, 1.5f})
        for (const float logical : {760.f, 560.f, 400.f, 300.f})
            for (const float extra : {0.f, 20.f, 60.f, 100.f})
                for (const bool banner : {false, true})
                    for (const std::size_t count : {std::size_t{2}, std::size_t{3}}) {
                        const auto shape = ShapeTableCards(logical * s, 1, false, s);
                        TableCardInput in;
                        in.content = {false, banner, true};
                        in.card = {0, 0, logical * s, shape.Least(in.content) + extra * s};
                        in.lead = 130 * s; in.words = 120 * s; in.score = 40 * s;
                        in.buttons.assign(count, 90 * s);
                        const auto at = LayOutTableCard(shape, in);
                        const char* step = Named(at.step);
                        const MenuRect inner{card::Pad * s, 0, (logical - card::Pad) * s, in.card.y1};
                        EXPECT_MSG(Inside(at.strip, inner) && Near(Height(at.strip), card::Strip * s) && Near(at.strip.y1, in.card.y1 - card::Foot * s) &&
                            Near(at.strip.x0, card::Pad * s) && Near(at.strip.x1, (logical - card::Pad) * s),
                            "scale %.1f width %.0f extra %.0f: the strip is not the card's inner width above its foot", s, logical, extra);
                        EXPECT_MSG(at.buttons.size() == count, "scale %.1f width %.0f: strip buttons lost", s, logical);
                        for (std::size_t i = 0; i < count; ++i) {
                            EXPECT_MSG(Near(at.buttons[i].y0, at.strip.y0) && Near(at.buttons[i].y1, at.strip.y1) && Near(Width(at.buttons[i]), Width(at.buttons[0])),
                                "scale %.1f width %.0f: strip buttons are not equal and as tall as the strip", s, logical);
                            if (i) EXPECT_MSG(Near(at.buttons[i].x0 - at.buttons[i - 1].x1, card::Between * s), "scale %.1f width %.0f: strip buttons are not a Between apart", s, logical);
                        }
                        EXPECT_MSG(Near(at.buttons.front().x0, at.strip.x0) && Near(at.buttons.back().x1, at.strip.x1), "scale %.1f width %.0f: strip buttons do not span the strip", s, logical);
                        // Below the seats, the pictures and the banner; apart from everything else.
                        EXPECT_MSG(at.strip.y0 >= (std::max)(at.seats[0].y1, at.seats[1].y1) + card::Between * s - .01f &&
                            (!banner || at.strip.y0 >= at.banner.y1 + card::Between * s - .01f), "scale %.1f width %.0f %s: the strip is not a Between under the seats and banner", s, logical, step);
                        for (std::size_t side = 0; side < 2; ++side)
                            EXPECT_MSG(Apart(at.strip, at.seats[side]) && Apart(at.strip, at.faces[side]) && at.faces[side].y1 <= at.strip.y0 + .01f,
                                "scale %.1f width %.0f %s: a seat or picture stood over the strip", s, logical, step);
                        EXPECT_MSG(Apart(at.strip, at.head) && Apart(at.strip, at.score) && Apart(at.strip, at.words) && Apart(at.strip, at.banner),
                            "scale %.1f width %.0f %s: the head, score, words or banner stood over the strip", s, logical, step);
                        // The card's line holds none of the buttons: its words run to the line's end.
                        EXPECT_MSG(Near(at.words.x1, shape.versus || !(in.score > 0) ? at.head.x1 : at.score.x0 - card::Space * s), "scale %.1f width %.0f: the line kept room for buttons", s, logical);
                    }
}

int main() {
    TestStackedCardKeepsItsText();
    TestSeatTagPlacesItsNote();
    TestCardLineHoldsTwoButtons();
    TestOwnCardStrip();
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
