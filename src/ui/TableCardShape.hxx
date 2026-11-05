#pragma once
// How a Classic room's table cards lie on the room board (RoomPanelTables.cxx
// draws them). Pure layout arithmetic in logical pixels, scaled by `s`.
//
// A card is the table's line (TABLE n, its state and its name), its two
// seats, an optional banner line under them, and the footer: the rules,
// queue and watchers, or the strip of buttons over them. A card wide enough
// is a versus line, as the Classic look had it before 1.5.0: a fighter's
// picture at each end, each player's lines between their picture and the
// score, and the score (or vs) small in the middle. A narrower card stacks
// the two seats, each picture beside its player's lines.
#include <algorithm>

namespace sf4e { namespace ui {
namespace table_card {
// The card's inner margin at its sides.
constexpr float Pad = 14;
// From the card's top to its seats: the table's line.
constexpr float Header = 44;
// From the seats to the footer, and the banner line that may sit between.
constexpr float Gap = 8;
constexpr float Banner = 18;
// The rules line, or the strip that covers it, and the margin under it.
constexpr float Footer = 44;
// A versus card's pictures, square, and the space between a picture and its player's lines.
constexpr float Face = 72;
constexpr float FaceGap = 10;
// The least a versus card leaves each player's lines (a name with its ping
// and link, and the fighter under it), and the score's place between them.
constexpr float SeatTextLeast = 200;
constexpr float Middle = 96;
// A stacked seat's row, and the space between the two rows.
constexpr float SeatRow = 40;
constexpr float SeatRowGap = 6;
// Between cards, across and down.
constexpr float Between = 8;
// The narrowest card that is a versus line.
constexpr float VersusLeast = 2 * Pad + 2 * (Face + FaceGap + SeatTextLeast) + Middle;
}

// `columns` across, each card a versus line or not, `height` tall.
struct TableCardShape { int columns; bool versus; float height; };

// The cards of a floor `width` wide: one per line (Settings › Room layout ›
// Tables), or `gridColumns` across with `grid`. `banners`: some table has a
// banner, so every card keeps its line (cards in a row stay one height).
inline TableCardShape ShapeTableCards(float width, int gridColumns, bool grid, bool banners, float s) {
    // Qualified: sf4e::ui has a Header of its own (Theme.hxx).
    namespace card = table_card;
    const int columns = grid ? (std::max)(1, gridColumns) : 1;
    const float cardWidth = (width - (columns - 1) * card::Between * s) / columns;
    const bool versus = cardWidth >= card::VersusLeast * s;
    const float seats = versus ? card::Face : 2 * card::SeatRow + card::SeatRowGap;
    return {columns, versus, (card::Header + seats + card::Gap + (banners ? card::Banner : 0) + card::Footer) * s};
}
} }
