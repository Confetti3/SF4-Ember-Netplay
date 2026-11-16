#pragma once
// How a Classic room's table cards lie on the room board (RoomPanelTables.cxx
// draws them; docs/design/ROOM_TABLES.md). Pure layout arithmetic in logical
// pixels, scaled by `s`.
//
// A card is as tall as what it holds. A quiet table (nobody seated, queued or
// watching) is one line: its head and its two seats beside it. Any other card
// is its head (the table's line), its two seats, an optional banner line under
// them, on your own place the strip of its buttons, and a foot. A card wide
// enough is a versus line, as the Classic look had it before 1.5.0: a
// fighter's picture at each end, each player's lines between their picture
// and the score, and the score (or vs) in the middle. A narrower card stacks
// the two seats, each picture beside its player's lines.
#include <algorithm>
#include <vector>

namespace sf4e { namespace ui {
namespace table_card {
// The card's inner margin at its sides.
constexpr float Pad = 12;
// The table's line: TABLE n, its state and name, and at its right what
// differs from the room.
constexpr float Head = 32;
// A versus card's pictures, square, and the space between a picture and its player's lines.
constexpr float Face = 56;
constexpr float FaceGap = 10;
// Under the seats, or under the strip.
constexpr float Foot = 8;
// The banner line under the seats.
constexpr float Banner = 18;
// Your own place's strip: its buttons and the space over them.
constexpr float Strip = 40;
constexpr float StripButton = 32;
// A quiet card's one line, and the seat buttons in it.
constexpr float Quiet = 48;
constexpr float QuietButton = 36;
// The least a seat button takes, and the least a quiet card's head takes beside two.
constexpr float OfferLeast = 140;
constexpr float QuietHeadLeast = 160;
// The least a versus card leaves each player's lines (a name with its ping
// and link, and the fighter under it), and the score's place between them.
constexpr float SeatTextLeast = 200;
constexpr float Middle = 96;
// A stacked seat's row, and the space between the two rows.
constexpr float SeatRow = 40;
constexpr float SeatRowGap = 4;
// Between cards, across and down; between the buttons on a card.
constexpr float Between = 8;
// The narrowest card that is a versus line.
constexpr float VersusLeast = 2 * Pad + 2 * (Face + FaceGap + SeatTextLeast) + Middle;
// The narrowest card whose quiet line holds its head and both seats.
constexpr float QuietLineLeast = 2 * Pad + QuietHeadLeast + 2 * (Between + OfferLeast);
}

// What a card holds: nobody at the table (`quiet`), a banner line, your own
// place's strip.
struct TableCardContent { bool quiet = false, banner = false, strip = false; };

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
        return (c::Head + seats + (card.banner ? c::Banner : 0) + (card.strip ? c::Strip : 0) + c::Foot) * s;
    }
};

// The cards of a floor `width` wide: one per line (Settings › Room layout ›
// Tables), or `gridColumns` across with `grid`.
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
} }
