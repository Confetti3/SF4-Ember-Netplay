#pragma once
namespace sf4e { namespace ui {
// The picture a room shows for a fighter (Settings › Room layout › Pictures):
// the fighter's whole figure in the costume and colour the player wears (the
// default), or the game's own select-screen portrait.
enum class FighterArt { Portrait, Character };
// A fighter as a room draws them. Characters shows the costume and colour; a
// look not known (-1) is drawn in the original outfit.
struct FighterPicture {
    int fighter = -1;
    FighterArt art = FighterArt::Portrait;
    int costume = 0, color = 0;
};
} }
