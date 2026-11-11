#pragma once
// The player's card from their fighter levels (docs/design/PLAYER_CARD.md): the
// fighters it shows and in what order, how far a level is, and the view Home's
// card and the Card tab draw from. The drawing is in PlayerCard.cxx; the
// choices are here so they are checked without a frame (player_card_test.cxx).
#include "GameMenu.hxx"
#include "../netplay/Answered.hxx"
#include "../netplay/Levels.hxx"
#include <cstdint>
#include <vector>

namespace sf4e { namespace ui { namespace card {
using Own = netplay::levels::Own;
using FighterLevel = netplay::levels::FighterLevel;

// The fighters with a level, highest first, then most games, then lowest id.
std::vector<int> PlayedFighters(const Own& own);
// A fighter's points: a game counts one and a win one more.
std::uint64_t Points(const FighterLevel& level);
// How far through its level the fighter is, 0 to 1; 1 at the top.
float Part(const FighterLevel& level);
// About how many more games reach the next level at this fighter's own points
// per game; 0 at the top or before any game.
std::uint64_t GamesAway(const FighterLevel& level);
// The card's levels from the answer: the main first, then the two highest
// others played, the main's progress and the facts. `main` is the fighter the
// player picked as their main (-1 for none).
void Fill(PlayerCardView& view, const netplay::Answered<Own>& levels, int main);
} } }
