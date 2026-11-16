#pragma once
// How a Classic room's table cards lie on the room board (RoomTableCards.cxx
// draws them; docs/design/ROOM_TABLES.md). Pure layout arithmetic, in logical
// pixels scaled by `s`, so every box is unit-tested.
//
// The cards fill the column (PlaceTableCards): every card the same height,
// the column's height shared among its rows, between the least a card takes
// (Compact) and the tallest it grows (table_card::Cap). Only where even that
// least does not fit do the cards keep their own least heights, then a quiet
// table (nobody seated, queued or watching, nothing to offer but its seats)
// becomes one line (Line), and only then does the list scroll.
//
// A card is its head (the table's line: TABLE n, its state, what differs from
// the room and its buttons), its body (the two seats), an optional banner
// line under them, and a foot. What the body shows steps with its height
// (TableCardStep): the pictures are as tall as the body allows, the words
// grow with them and the lines the compact card leaves out come back. A card
// wide enough is a versus line: a fighter's picture at each outer end, each
// player's lines beside it, and the score (or vs) in the middle; a narrower
// card stacks the two seats, each picture beside its player's lines.
#include "MenuNavigation.hxx"
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {
namespace table_card {
// The card's inner margin at its sides.
constexpr float Pad = 12;
// The table's line, and how far its buttons stand inside it.
constexpr float Head = 28;
constexpr float HeadInset = 3;
// A Compact versus card's pictures, square, and the space between a picture and its player's lines.
constexpr float Face = 48;
constexpr float FaceGap = 8;
// Under the seats.
constexpr float Foot = 8;
// The banner line under the seats.
constexpr float Banner = 18;
// A quiet card's one line, and the seat buttons in it.
constexpr float Quiet = 44;
constexpr float QuietButton = 32;
// The least a seat button takes, and the least a quiet card's head takes beside two.
constexpr float OfferLeast = 140;
constexpr float QuietHeadLeast = 160;
// The least a versus card leaves each player's lines (a name with its ping
// and link, and the fighter under it), and the score's place between them.
constexpr float SeatTextLeast = 200;
constexpr float Middle = 96;
// A Compact stacked seat's row, and the space between the two rows.
constexpr float SeatRow = 36;
constexpr float SeatRowGap = 4;
// Between cards, across and down, and between the buttons on a card.
constexpr float Between = 6;
// Between the words on a card's line and what stands beside them.
constexpr float Space = 8;
// Where the steps begin, by the height of one seat's place (a versus card's
// body, a stacked card's row): Medium at 84 (a versus card 120 tall), Roomy
// at 124 (160 tall).
constexpr float MediumSeat = 84;
constexpr float RoomySeat = 124;
// The tallest a card grows to fill the column: its pictures then 172 px.
constexpr float Cap = 208;
// The narrowest card that is a versus line.
constexpr float VersusLeast = 2 * Pad + 2 * (Face + FaceGap + SeatTextLeast) + Middle;
// The narrowest card whose quiet line holds its head and both seats.
constexpr float QuietLineLeast = 2 * Pad + QuietHeadLeast + 2 * (Between + OfferLeast);
}

// What a card holds: nobody at the table (`quiet`), a banner line.
struct TableCardContent { bool quiet = false, banner = false; };

// What a card shows at its height, least first. Line: a quiet table's one
// line (its head and two seat buttons). Compact: pictures 48 px and more, the
// player's name and fighter, a win streak (else the idle time) beside the name
// where it fits.
// Medium: the streak (else the idle time or record) on a line of its own.
// Roomy: the name in the heading size, and both lines under the fighter.
enum class TableCardStep { Line, Compact, Medium, Roomy };
// The sizes of a step's words, in logical pixels: the score (or vs) in the
// heading font, the fighter's line, a seat button's label, the lines under
// the fighter (a streak, then the idle time or record), and whether the name
// is the heading-size tag (v2::TagSize::M).
struct TableCardType {
    float score, fighter, offer;
    int notes;
    bool largeName;
};
inline TableCardType TypeOf(TableCardStep step) {
    switch (step) {
    case TableCardStep::Line: return {0, 13, 15, 0, false};
    case TableCardStep::Compact: return {18, 13, 15, 0, false};
    case TableCardStep::Medium: return {20, 14, 15, 1, false};
    case TableCardStep::Roomy: break;
    }
    return {22, 15, 17, 2, true};
}

// `columns` across, each card a versus line or not, a quiet line one line or two.
struct TableCardShape {
    int columns;
    bool versus, quietLine;
    float s;
    // The least a card takes as a whole card (Compact): its head, its seats at
    // their least, its banner and its foot.
    float Least(const TableCardContent& card) const {
        namespace c = table_card;
        const float seats = versus ? c::Face : 2 * c::SeatRow + c::SeatRowGap;
        return (c::Head + seats + (card.banner ? c::Banner : 0) + c::Foot) * s;
    }
    // A quiet card's line: one line, or its head over its seats where the card is too narrow.
    float Line() const {
        namespace c = table_card;
        return (quietLine ? c::Quiet : c::Head + c::QuietButton + c::Foot) * s;
    }
    // The height of one seat's place in a card `height` tall: a versus card's body, a stacked card's row.
    float SeatHeight(const TableCardContent& card, float height) const {
        namespace c = table_card;
        const float body = height - (c::Head + c::Foot + (card.banner ? c::Banner : 0)) * s;
        return versus ? body : (body - c::SeatRowGap * s) * .5f;
    }
    // The step of a card `height` tall: a quiet card shorter than a whole card is its line.
    TableCardStep StepOf(const TableCardContent& card, float height) const {
        namespace c = table_card;
        if (card.quiet && height < Least(card) - .01f) return TableCardStep::Line;
        const float seat = SeatHeight(card, height) / s;
        return seat >= c::RoomySeat - .01f ? TableCardStep::Roomy : seat >= c::MediumSeat - .01f ? TableCardStep::Medium : TableCardStep::Compact;
    }
};

// The cards of a floor `width` wide, as drawn (less the scroll's bar when it
// shows one): one per line (Settings › Room layout › Tables), or
// `gridColumns` across with `grid`.
inline TableCardShape ShapeTableCards(float width, int gridColumns, bool grid, float s) {
    // Qualified: sf4e::ui has a Header of its own (Theme.hxx).
    namespace card = table_card;
    const int columns = grid ? (std::max)(1, gridColumns) : 1;
    const float cardWidth = (width - (columns - 1) * card::Between * s) / columns;
    return {columns, cardWidth >= card::VersusLeast * s, cardWidth >= card::QuietLineLeast * s, s};
}

// Where the cards stand down a floor `area` tall, in their order: each one's
// top (from the floor's) and height, the floor's height and whether it is
// taller than `area` (the list scrolls); and, whatever `area`, the cards at
// their least: the tallest row (the least the board keeps for one row) and
// the whole floor.
struct TableCardPlaces {
    std::vector<float> tops, heights;
    float height = 0, leastRow = 0, leastHeight = 0;
    bool scrolls = false;
};
// The fill rule. With R rows (a grid's cards in rows of `columns`) and every
// card at least its Compact height, each row is (area - gaps) / R tall, in
// whole pixels, at most Cap: every card the same height, the column filled,
// what the cap leaves over below the last row. Where that is less than the
// tallest card's least, each card takes its own least (a row its tallest's);
// where that does not fit either, a quiet card is its Line; and then the list
// scrolls.
inline TableCardPlaces PlaceTableCards(const TableCardShape& shape, const std::vector<TableCardContent>& cards, float area) {
    namespace card = table_card;
    TableCardPlaces places;
    if (cards.empty()) return places;
    const std::size_t across = static_cast<std::size_t>((std::max)(1, shape.columns)), rows = (cards.size() + across - 1) / across;
    const float gap = card::Between * shape.s, gaps = (rows - 1) * gap;
    // The rows' heights for each card's own height, and their sum with the gaps.
    const auto stand = [&](const auto& height) {
        std::vector<float> row(rows, 0.f);
        for (std::size_t i = 0; i < cards.size(); ++i) row[i / across] = (std::max)(row[i / across], height(cards[i]));
        float total = gaps;
        for (const float h : row) total += h;
        return std::make_pair(row, total);
    };
    float tallest = 0;
    for (const auto& c : cards) tallest = (std::max)(tallest, shape.Least(c));
    const auto least = stand([&](const TableCardContent& c) { return shape.Least(c); });
    const auto lines = stand([&](const TableCardContent& c) { return c.quiet ? shape.Line() : shape.Least(c); });
    places.leastHeight = lines.second;
    places.leastRow = *std::max_element(lines.first.begin(), lines.first.end());
    const float each = std::floor((area - gaps) / rows);
    std::vector<float> row;
    if (each >= tallest) row.assign(rows, (std::min)(each, (std::max)(card::Cap * shape.s, tallest)));
    else row = least.second <= area ? least.first : lines.first;
    float y = 0;
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t i = r * across; i < (std::min)(cards.size(), (r + 1) * across); ++i) { places.tops.push_back(y); places.heights.push_back(row[r]); }
        places.height = y + row[r];
        y += row[r] + gap;
    }
    places.scrolls = places.height > area + .5f;
    return places;
}

// A seat's picture, square: as tall as the seat's place (`height`) allows,
// leaving the player's lines (a seat `width` wide, less the picture and its
// gap) SeatTextLeast, but never under a Compact picture. A versus card's seat
// and a stacked card's row share this one rule.
inline float FaceSize(float width, float height, float s) {
    namespace card = table_card;
    return (std::min)(height, (std::max)((std::min)(card::Face * s, height), width - (card::FaceGap + card::SeatTextLeast) * s));
}

// Where the x of a Compact seat's tag line puts its name's tag and the note
// beside it (a streak chip, else the idle time): both when they fit the seat's
// lines `room` wide from `left` with a space between, else the tag alone. A
// versus card centres the pair between the picture and the score; the second
// seat's pair mirrors, its note on the tag's inner side (towards the score).
struct SeatTag {
    float tag = 0, note = 0;
    bool noted = false;
};
inline SeatTag PlaceSeatTag(float left, float room, float tagWidth, float noteWidth, bool versus, bool mirrored, float s) {
    const float beside = noteWidth > 0 ? noteWidth + table_card::Space * s : 0;
    SeatTag at;
    at.noted = beside > 0 && tagWidth + beside <= room;
    const float total = tagWidth + (at.noted ? beside : 0), start = left + (versus ? (std::max)(0.f, room - total) * .5f : 0.f);
    at.tag = mirrored && at.noted ? start + beside : start;
    at.note = mirrored ? start : start + tagWidth + table_card::Space * s;
    return at;
}

// What a card's boxes are laid out from, in screen pixels: its box (whose
// height decides its step), what it holds, the width its head's lead takes
// (TABLE n, its state and their spaces), the words that follow want (its
// name, rules and counts), its score's width (0 for none) and its buttons'
// wanted widths, left to right: on its line, or on a quiet card, which has
// none there, a question's answers.
struct TableCardInput {
    MenuRect card;
    TableCardContent content;
    float lead = 0, words = 0, score = 0;
    std::vector<float> buttons;
};
// The card's boxes: its step; its line; its two seats (a player's picture and
// lines, a seat to take, or "Open seat"); each seat's picture, square, at its
// outer end, as tall as the seat's place allows; its score's place (empty for
// none); its buttons' press boxes, left to right: on its line between its
// words and its line's end, never over its lead, the line's whole height
// (drawn HeadInset inside it), as wide as they want or sharing what is left;
// on a quiet card a question's answers across its seats' place, the seats
// then standing aside; its words' place; its banner's (empty for none).
struct TableCardLayout {
    TableCardStep step = TableCardStep::Compact;
    MenuRect head;
    std::array<MenuRect, 2> seats, faces;
    MenuRect score, words, banner;
    std::vector<MenuRect> buttons;
};
inline TableCardLayout LayOutTableCard(const TableCardShape& shape, const TableCardInput& in) {
    namespace card = table_card;
    const float s = shape.s;
    const MenuRect& b = in.card;
    const float left = b.x0 + card::Pad * s, right = b.x1 - card::Pad * s, inner = right - left;
    TableCardLayout at;
    at.step = shape.StepOf(in.content, b.y1 - b.y0);
    // A question's answers across the span x to `right`, from `top`, `height` tall; the seats stand aside.
    const auto answers = [&](float x, float top, float height) {
        const std::size_t count = in.buttons.size();
        if (!count) return;
        const float gap = card::Between * s, each = (std::max)(0.f, (right - x - (count - 1) * gap) / count);
        at.buttons.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            const float x0 = x + i * (each + gap);
            at.buttons[i] = {x0, top, x0 + each, top + height};
        }
        at.seats[0] = at.seats[1] = {right, top, right, top + height};
    };
    if (at.step == TableCardStep::Line) {
        // One line: the lead and its words, then the two seats sharing the rest,
        // each at least a seat's least. Narrower, the seats go under the head.
        const bool line = shape.quietLine;
        const float wanted = (std::max)(card::QuietHeadLeast * s, in.lead + (in.words > 0 ? in.words + card::Space * s : 0));
        // The lead gives way before either seat goes under its least: two seats, a space between and one before them.
        const float lead = line ? (std::max)(0.f, (std::min)(wanted, inner - 2 * card::OfferLeast * s - 2 * card::Between * s)) : 0;
        const float height = line ? b.y1 - b.y0 : card::Head * s, top = line ? b.y0 + (height - card::QuietButton * s) * .5f : b.y0 + height;
        const float x = left + lead + (line ? card::Between * s : 0), half = (right - x - card::Between * s) * .5f;
        at.seats[0] = {x, top, x + half, top + card::QuietButton * s};
        at.seats[1] = {x + half + card::Between * s, top, right, top + card::QuietButton * s};
        at.head = {left, b.y0, line ? x - card::Space * s : right, b.y0 + height};
        at.words = {(std::min)(left + in.lead, at.head.x1), at.head.y0, at.head.x1, at.head.y1};
        // A question on a quiet card (a watcher's seat that would end the view): its answers take the seats' span.
        answers(x, top, card::QuietButton * s);
        return at;
    }
    at.head = {left, b.y0, right, b.y0 + card::Head * s};
    const float top = at.head.y1, bottom = b.y1 - (card::Foot + (in.content.banner ? card::Banner : 0)) * s, body = (std::max)(0.f, bottom - top);
    if (in.content.quiet) {
        // Nobody at the table: its two seats share the body, side by side, each a half.
        const float half = (inner - card::Between * s) * .5f;
        at.seats[0] = {left, top, left + half, top + body};
        at.seats[1] = {right - half, top, right, top + body};
        at.words = {(std::min)(left + in.lead, right), at.head.y0, right, at.head.y1};
        // A question's answers where the seats' buttons are, across the card.
        answers(left, top, body);
        return at;
    }
    if (shape.versus) {
        const float middle = (std::max)(card::Middle * s, in.score + 4 * card::Space * s), half = (inner - middle) * .5f;
        at.seats[0] = {left, top, left + half, top + body};
        at.seats[1] = {right - half, top, right, top + body};
        if (in.score > 0) at.score = {at.seats[0].x1, top, at.seats[1].x0, top + body};
        // As tall as the body, leaving each player's lines their least; never under a Compact picture.
        const float face = FaceSize(half, body, s);
        const float faceTop = top + (body - face) * .5f;
        at.faces[0] = {left, faceTop, left + face, faceTop + face};
        at.faces[1] = {right - face, faceTop, right, faceTop + face};
    } else {
        // Its picture beside the lines, at most half the card, leaving them their least as a versus card does.
        const float row = (std::max)(0.f, (body - card::SeatRowGap * s) * .5f), face = (std::min)(FaceSize(inner, row, s), inner * .5f);
        for (std::size_t side = 0; side < 2; ++side) {
            const float y = top + side * (row + card::SeatRowGap * s);
            at.seats[side] = {left, y, right, y + row};
            at.faces[side] = {left, y + (row - face) * .5f, left + face, y + (row + face) * .5f};
        }
        if (in.score > 0) at.score = {right - in.score, at.head.y0, right, at.head.y1};
    }
    // The head's buttons from its end leftwards, never over its lead: where
    // they do not all fit at their width they share the room, their labels cut.
    const float end = in.score > 0 && !shape.versus ? at.score.x0 - card::Space * s : right, start = (std::min)(left + in.lead, end);
    const std::size_t count = in.buttons.size();
    float wanted = count ? (count - 1) * card::Between * s : 0;
    for (const float w : in.buttons) wanted += w;
    const float room = end - start, share = count ? (std::max)(0.f, (room - (count - 1) * card::Between * s) / count) : 0;
    float x = end;
    at.buttons.resize(count);
    for (std::size_t i = count; i-- > 0;) {
        const float w = wanted <= room ? in.buttons[i] : share;
        at.buttons[i] = {x - w, at.head.y0, x, at.head.y1};
        x -= w + card::Between * s;
    }
    const float wordsEnd = count ? (std::max)(start, at.buttons.front().x0 - card::Space * s) : end;
    at.words = {start, at.head.y0, wordsEnd, at.head.y1};
    if (in.content.banner) at.banner = {left, bottom, right, bottom + card::Banner * s};
    return at;
}
} }
