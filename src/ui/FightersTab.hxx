#pragma once
// The Fighters tab's grid and panel (docs/design/PROFILE.md, "Fighters tab (own)" and "Their Fighters tab"): every fighter
// in two groups, the played ones (most points first) and the rest, each a tile with its picture, and beside the grid the
// focused fighter's panel. The player's own tab (ProfilePanel) and another's (PlayerPage) draw with this; another's
// has no games, record, meters or Save line, because their games per fighter are never served.
#include "GameMenu.hxx"
#include "FighterArt.hxx"
#include "MenuNavigation.hxx"
#include "../netplay/Levels.hxx"
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace fighters_tab {

struct Shown {
    // Every fighter's level, indexed by fighter; a fighter with no points is not played.
    const netplay::levels::Own* levels = nullptr;
    int main = -1;
    FighterArt art = FighterArt::Portrait;
    // 0.4 while the points are read.
    float alpha = 1.f;
    // Another player's tab: no games under a tile, no record, no meter, no Save line.
    bool others = false;
    // The catalog id of the words that say why there are no points, or null (the player's own tab).
    const char* why = nullptr;
};

// The row id a fighter's tile has ("main-<fighter>"): Select on it makes it the main on the player's own tab.
constexpr const char* RowPrefix = "main-";

// The tab at the cursor: `rows` are the fighters' rows (and Connect Discord), placed where the tiles are drawn. Returns the
// height taken.
float Draw(const Shown& shown, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed);

// Another player's levels as the tab draws them: the card's fighters at their points and tiers, the others at 0.
netplay::levels::Own LevelsOfCard(const netplay::levels::Card& card);

} } }
