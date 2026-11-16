#pragma once
// How a Classic room's table cards lie on the room board (RoomTableCards.cxx
// draws them; docs/design/ROOM_TABLES.md). Pure layout arithmetic, in logical
// pixels scaled by `s`, so every box is unit-tested.
//
// A card is as tall as what it holds. A quiet table (nobody seated, queued or
// watching, and nothing to offer but its seats) is one line: its head and its
// two seats beside it. Any other card is its head (the table's line: TABLE n,
// its state, what differs from the room and its buttons), its two seats, an
// optional banner line under them, and a foot. A card wide enough is a versus
// line: a fighter's picture at each end, each player's lines between their
// picture and the score, and the score (or vs) in the middle. A narrower card
// stacks the two seats, each picture beside its player's lines.
#include "MenuNavigation.hxx"
#include <algorithm>
#include <array>
#include <vector>

namespace sf4e { namespace ui {
namespace table_card {
// The card's inner margin at its sides.
constexpr float Pad = 12;
// The table's line, and how far its buttons stand inside it.
constexpr float Head = 28;
constexpr float HeadInset = 3;
// A versus card's pictures, square, and the space between a picture and its player's lines.
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
// A stacked seat's row, and the space between the two rows.
constexpr float SeatRow = 36;
constexpr float SeatRowGap = 4;
// Between cards, across and down, and between the buttons on a card.
constexpr float Between = 6;
// Between the words on a card's line and what stands beside them.
constexpr float Space = 8;
// The narrowest card that is a versus line.
constexpr float VersusLeast = 2 * Pad + 2 * (Face + FaceGap + SeatTextLeast) + Middle;
// The narrowest card whose quiet line holds its head and both seats.
constexpr float QuietLineLeast = 2 * Pad + QuietHeadLeast + 2 * (Between + OfferLeast);
}

// What a card holds: nobody at the table (`quiet`), a banner line.
struct TableCardContent { bool quiet = false, banner = false; };

// `columns` across, each card a versus line or not, a quiet card one line or two.
struct TableCardShape {
    int columns;
    bool versus, quietLine;
    float s;
    // A card's height for what it holds.
    float Height(const TableCardContent& card) const {
        namespace c = table_card;
        if (card.quiet) return (quietLine ? c::Quiet : c::Head + c::QuietButton + c::Foot) * s;
        const float seats = versus ? c::Face : 2 * c::SeatRow + c::SeatRowGap;
        return (c::Head + seats + (card.banner ? c::Banner : 0) + c::Foot) * s;
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

// Where the cards stand down the floor, in their order: each one's top (from
// the floor's) and height, the cards of a row one height (its tallest's), and
// the floor's height and its tallest row.
struct TableCardPlaces { std::vector<float> tops, heights; float height = 0, tallest = 0; };
inline TableCardPlaces PlaceTableCards(const TableCardShape& shape, const std::vector<TableCardContent>& cards) {
    namespace card = table_card;
    TableCardPlaces places;
    const std::size_t across = static_cast<std::size_t>(shape.columns);
    float y = 0;
    for (std::size_t first = 0; first < cards.size(); first += across) {
        const std::size_t end = (std::min)(cards.size(), first + across);
        float row = 0;
        for (std::size_t i = first; i < end; ++i) row = (std::max)(row, shape.Height(cards[i]));
        for (std::size_t i = first; i < end; ++i) { places.tops.push_back(y); places.heights.push_back(row); }
        places.tallest = (std::max)(places.tallest, row);
        places.height = y + row;
        y += row + card::Between * shape.s;
    }
    return places;
}

// What a card's boxes are laid out from, in screen pixels: its box, what it
// holds, the width its head's lead takes (TABLE n, its state and their
// spaces), the words that follow want (its name, rules and counts), its
// score's width (0 for none) and its head buttons' wanted widths, left to right.
struct TableCardInput {
    MenuRect card;
    TableCardContent content;
    float lead = 0, words = 0, score = 0;
    std::vector<float> buttons;
};
// The card's boxes: its line; its two seats; its score's place (empty for
// none); its head buttons, left to right, between its words and its line's
// end, never over its lead, as wide as they want or sharing what is left;
// its words' place; its banner's (empty for none).
struct TableCardLayout {
    MenuRect head;
    std::array<MenuRect, 2> seats;
    MenuRect score, words, banner;
    std::vector<MenuRect> buttons;
};
inline TableCardLayout LayOutTableCard(const TableCardShape& shape, const TableCardInput& in) {
    namespace card = table_card;
    const float s = shape.s;
    const MenuRect& b = in.card;
    const float left = b.x0 + card::Pad * s, right = b.x1 - card::Pad * s, inner = right - left;
    TableCardLayout at;
    if (in.content.quiet) {
        // One line: the lead and its words, then the two seats sharing the rest,
        // each at least a seat's least. Narrower, the seats go under the head.
        const bool line = shape.quietLine;
        const float wanted = (std::max)(card::QuietHeadLeast * s, in.lead + (in.words > 0 ? in.words + card::Space * s : 0));
        const float lead = line ? (std::min)(wanted, inner - 2 * card::OfferLeast * s - card::Between * s) : 0;
        const float height = (line ? card::Quiet : card::Head) * s, top = line ? b.y0 + (card::Quiet - card::QuietButton) * .5f * s : b.y0 + height;
        const float x = left + lead + (line ? card::Between * s : 0), half = (right - x - card::Between * s) * .5f;
        at.seats[0] = {x, top, x + half, top + card::QuietButton * s};
        at.seats[1] = {x + half + card::Between * s, top, right, top + card::QuietButton * s};
        at.head = {left, b.y0, line ? x - card::Space * s : right, b.y0 + height};
        at.words = {(std::min)(left + in.lead, at.head.x1), at.head.y0, at.head.x1, at.head.y1};
        // A question on a quiet card (a watcher's seat that would end the view): its answers take the seats' place.
        if (const std::size_t count = in.buttons.size()) {
            const float gap = card::Between * s, each = (right - x - (count - 1) * gap) / count;
            at.buttons.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                const float x0 = x + i * (each + gap);
                at.buttons[i] = {x0, top, x0 + each, top + card::QuietButton * s};
            }
            at.seats[0] = at.seats[1] = {right, top, right, top};
        }
        return at;
    }
    at.head = {left, b.y0, right, b.y0 + card::Head * s};
    const float seatTop = at.head.y1;
    if (shape.versus) {
        const float middle = (std::max)(card::Middle * s, in.score + 4 * card::Space * s), half = (inner - middle) * .5f;
        at.seats[0] = {left, seatTop, left + half, seatTop + card::Face * s};
        at.seats[1] = {right - half, seatTop, right, seatTop + card::Face * s};
        if (in.score > 0) at.score = {at.seats[0].x1, seatTop, at.seats[1].x0, at.seats[1].y1};
    } else {
        for (int side = 0; side < 2; ++side) {
            const float top = seatTop + side * (card::SeatRow + card::SeatRowGap) * s;
            at.seats[static_cast<std::size_t>(side)] = {left, top, right, top + card::SeatRow * s};
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
        at.buttons[i] = {x - w, at.head.y0 + card::HeadInset * s, x, at.head.y1 - card::HeadInset * s};
        x -= w + card::Between * s;
    }
    const float wordsEnd = count ? (std::max)(start, at.buttons.front().x0 - card::Space * s) : end;
    at.words = {start, at.head.y0, wordsEnd, at.head.y1};
    if (in.content.banner) at.banner = {left, at.seats[1].y1, right, at.seats[1].y1 + card::Banner * s};
    return at;
}
} }
