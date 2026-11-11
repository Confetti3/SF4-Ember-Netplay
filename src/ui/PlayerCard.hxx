#pragma once
// The player's card from their fighter points (docs/design/PLAYER_CARD.md): the
// fighters it shows and in what order, how far the next tier is, and the view Home's
// card and the Card tab draw from. The drawing is in PlayerCard.cxx; the
// choices are here so they are checked without a frame (player_card_test.cxx).
#include "GameMenu.hxx"
#include "../netplay/Answered.hxx"
#include "../netplay/Levels.hxx"
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace player_card {
using Own = netplay::levels::Own;
using FighterLevel = netplay::levels::FighterLevel;

// The fighters with points, highest first, then lowest id.
std::vector<int> PlayedFighters(const Own& own);
// How far the fighter's points are to the next tier's start, 0 to 1 (the
// bridge sends no earlier start); 1 at the top.
float Part(const FighterLevel& level);
// The card's points from the answer: the main first, then the two highest
// others played, the main's progress and the facts. `main` is the fighter the
// player picked as their main (-1 for none).
void Fill(PlayerCardView& view, const netplay::Answered<Own>& levels, int main);
// The catalog id of the words that say why the card has no points to show, or
// null when it has (no answer yet, no account, a service without points, nothing played).
const char* NoLevelWords(const PlayerCardView& view);
// "1 game" or "N games": the catalogs have no plural forms, so one has its own string.
std::string GamesWords(std::uint64_t games);
}

struct ShellView;
// The row of a fighter in the Card tab's strip, and the row that holds the
// words when there are no points to show (Card and Fighters tabs).
inline std::string CardFighterRow(int fighter) { return "card-fighter:" + std::to_string(fighter); }
constexpr const char* ProfileStateRow = "profile-state";
// The row `id` among `rows` as a Primary button `width` wide at `at` (Connect
// Discord), pressed through PressEntry. Nothing when there is no such row.
// Returns the bottom of what it drew, `at.y` when nothing.
float DrawRowButton(const char* id, ImVec2 at, float width, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed);
// The Card tab's board at the cursor, `width` wide: the hero, the plates, the
// player's words, meter and facts, and the strip, whose tiles are the `rows`
// (CardFighterRow), pressed through PressEntry. `main` is the fighter shown
// large; `changed` scrolls a newly focused row into view. Returns the height taken.
float DrawCardTab(const ShellView& view, int main, const PlayerCardView& card, const std::vector<MenuEntry>& rows, MenuNavigation& nav,
    MenuAction& action, bool changed);
} }
