#pragma once
// One family's tile on the awards wall (docs/design/PROFILE.md, "Awards tab (own)" and "Their Awards tab"): its icon on
// the plate of the level reached, the family's name or title, the level pips and how far the next level is. The player's
// own wall (ApplicationShell__Awards.cxx) and another player's, read only (PlayerPage), draw their tiles with this, so a
// family looks the same on both.
#include "V2.hxx"
#include "../netplay/Awards.hxx"
#include <imgui.h>
#include <string>

namespace sf4e { namespace ui { namespace awards_wall {

struct Tile {
    const netplay::awards::Family* family = nullptr;
    // The level the counters reach (0: none) and the way to the next.
    int level = 0;
    netplay::awards::Step next;
    // The words under the icon: the current title, or the family's name while none is reached.
    std::string name;
    // The counters are read (a locked family then says "Locked" until they are).
    bool counted = false;
    // The family is worn on the player's own card: its tile has a thin accent outline and its words in the accent.
    bool worn = false;
    // False when the tile's row is disabled: its words are muted.
    bool live = true;
};

// How a level is reached, in words: "Reach class A" for a class level, else the level's own.
std::string LevelHow(const netplay::awards::Level& level);
// A family's levels, one line each: tier, title, how, and "Earned" on the ones reached or how far along the next is.
std::string LevelLines(const netplay::awards::Counters& counters, const netplay::awards::Family& family, bool counted);
// The line under the pips: "Top level", "Locked", "712 of 1000" for a count, or the next level's title for a step.
std::string StateOf(const Tile& tile);
// The tile in `box`; `focused` draws the focus and `hovered` the pointer's ground.
void DrawTile(ImDrawList* d, v2::Box box, const Tile& tile, bool focused, bool hovered);

} } }
