#pragma once
// Rankings' two boards, drawn in place of the menu's list (GameMenu::Body) as
// a side list and the page it picks, for the pad: the classes down the side
// and the players of the class on show beside them; the fighters down the
// side and the points board of the fighter on show, as the site's table. Up and
// Down walk a column, Right or A goes into the page, Left or Back returns to
// the side (RankingsPanel::ListFocused). The rows (RankingsPanel::Rows) stay
// the focus and the presses; these only place and draw them.
#include "GameMenu.hxx"
#include "../common/SkillBadge.hxx"
#include "../netplay/Rankings.hxx"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sf4e { namespace ui {
// The boards' row ids, made and read only here. The side lists: a class, a
// fighter (-1: every fighter). The pages: a player by their index on the board.
std::string ClassRow(std::uint8_t band);
std::string FighterRow(int fighter);
std::string BoardPlayerRow(std::size_t at);
std::string PointsPlayerRow(std::size_t at);
std::optional<std::uint8_t> ClassOfRow(std::string_view id);
std::optional<int> FighterOfRow(std::string_view id);
std::optional<std::size_t> BoardPlayerOfRow(std::string_view id);
std::optional<std::size_t> PointsPlayerOfRow(std::string_view id);
// How many players the board lists in each class (`of[band]`), and the most any class has, at least 1.
struct ClassCounts {
    std::array<int, SkillBadge::Classes + 1> of{};
    int most = 1;
};
ClassCounts CountClasses(const std::vector<netplay::rankings::BoardEntry>& board);

struct RankingsBoardFrame {
    const std::vector<MenuEntry>& rows;
    // Its navigation, and its own drawing of a row the board has no look for (a state row).
    GameMenu& menu;
    MenuAction& action;
    // The focus moved since the last frame: the board scrolls it into view.
    bool changed = false;
};
// The class board, `board` null until it loads; `ownBand` is the class the You
// row shows, 0 for none; `shown` is the class whose players `rows` carry.
void DrawClassBoard(const RankingsBoardFrame& f, const std::vector<netplay::rankings::BoardEntry>* board, std::uint8_t ownBand, std::uint8_t shown);
// The points board for `fighter` (-1: every fighter), `board` null until it loads; `own` is how boards list this player.
void DrawPointsBoard(const RankingsBoardFrame& f, const netplay::rankings::PointsBoard* board, int fighter, const netplay::rankings::Listing& own);
} }
