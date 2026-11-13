#pragma once
// The player's card in three sizes (docs/design/PROFILE.md): Full (a Card tab,
// the player's own or another's), the Panel (beside a list) and the Row. What
// each shows and in what order is CardModel.hxx, checked without a frame
// (player_card_test.cxx); the parts are CardParts.hxx. This file holds the
// points helpers Home's card and the Fighters tab read, and the drawing of the
// Full card and the Panel, which only draw what the view models say.
#include "CardModel.hxx"
#include "GameMenu.hxx"
#include "../netplay/Answered.hxx"
#include "../netplay/FriendsView.hxx"
#include "../netplay/Levels.hxx"
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace player_card {
using Own = netplay::levels::Own;
using Level = netplay::levels::Level;

// The fighters with points, highest first, then lowest id.
std::vector<int> PlayedFighters(const Own& own);
// Every fighter in the Fighters tab's two groups: the played ones as
// PlayedFighters has them, then the rest in roster order. The main leads its
// own group. `own` with nothing played puts all of them in `unplayed`.
struct Groups { std::vector<int> played, unplayed; };
Groups Grouped(const Own& own, int main);
// How far the fighter's points are to the next tier's start, 0 to 1 (the
// bridge sends no earlier start); 1 at the top.
float Part(const Level& level);
// The card's points from the answer: the main first, then the two highest
// others played, the main's and the account's counts and the facts. `main` is
// the fighter the player picked as their main (-1 for none). A read in
// flight or failed after an answer shows that answer.
void Fill(PlayerCardView& view, const netplay::Answered<Own>& levels, int main);
// The catalog id of the words that say why the card has no points to show, or
// null when it has (no answer yet, no account, a service without points, one
// that could not be reached, nothing played).
const char* NoLevelWords(const PlayerCardView& view);
// One of the Fighters tab's progress lines: "Ryu · Gold" or "Account · Copper",
// how far to the next tier (0 to 1), and the words at its right ("514 points
// to Ember", "Top frame", "Not played yet"). A level with no points is only
// named: it has no frame yet, and an account at 0 still says what its first
// frame needs.
struct Progress { std::string label, words; float part = 0; };
Progress ProgressOf(const std::string& name, const Level& level, bool account);
// The main's row (none without a main), then the account's.
std::vector<Progress> ProgressRows(const PlayerCardView& view);
// "1 game" or "N games": the catalogs have no plural forms, so one has its own string.
std::string GamesWords(std::uint64_t games);
}

struct ShellView;
// The row of a fighter in the Card tab's strip, the row that opens Edit card
// (also the page's name), and the rows that open Connect Discord and the main
// page (ids their screens go by).
inline std::string CardFighterRow(int fighter) { return "card-fighter:" + std::to_string(fighter); }
constexpr const char* CardEditRow = "card-edit";
constexpr const char* ChooseMainRow = "main-character";
// The row that opens Fighter select from the Card tab (the id of its screen).
constexpr const char* FighterSelectRow = "selection";
// The row that opens Connect Discord (a Primary button on the tabs that need an account).
MenuEntry ConnectDiscordRow();
// The row `id` among `rows` as a Primary button `width` wide at `at` (Connect
// Discord), pressed through PressEntry. Nothing when there is no such row.
// Returns the bottom of what it drew, `at.y` when nothing.
float DrawRowButton(const char* id, ImVec2 at, float width, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action, bool changed);
// A fighter's picture line: the tier as a 1 px line in its stripe colour, 3 px
// inside `box`, once the fighter has points; nothing otherwise. The thick
// accent frame is focus and nothing else.
void DrawFighterEdge(ImDrawList* d, v2::Box box, std::uint64_t points, int tier);

// The Full card at the cursor, as wide as the window and, wide or compact, as
// tall as the body (`bodyHeight`): the hero the body's height with its buttons
// at its foot, then the middle column and the right column, each to the foot,
// nothing scrolling; narrow, one column that scrolls. The parts are placed
// rows (PressEntry): the strip's tiles, the award tiles, Most played, the set
// rows, All sets (player_card::CardRows), the hero's buttons and Choose a main
// among `rows`. `changed` scrolls a newly focused row into view. Returns the
// height taken.
float DrawFullCard(const player_card::FullCardView& card, FighterArt art, const std::vector<MenuEntry>& rows, MenuNavigation& nav, MenuAction& action,
    bool changed, float bodyHeight);
// The size class of a body `bodyHeight` tall (physical px) at the current window and scale: what FullOf takes.
player_card::Size CardSize(float bodyHeight);

// Another player's card as the Panel draws it (a room member, a friend, the
// player's own as others see it): the picture of their fighter on its tier
// line, their flag, name and class, the worn title on its plate and the worn
// icon beside it, `<n> FP` of that fighter and `<n> BP` of the account under
// the plate, the record line, Plays and Looking for on lines of their own, the
// award families' icons on their tier plates, and a chip for how they stand
// with the viewer. A part the data does not have is left out.
struct OtherCardView {
    std::string name;
    CountryCode country;
    SkillBadge badge;
    // The worn title on its tier's plate and the worn icon beside it, each at the level the card's counters earn;
    // none when empty or -1.
    std::string title;
    v2::Tier titleTier = v2::Tier::None;
    int icon = -1;
    v2::Tier iconTier = v2::Tier::None;
    int fighter = -1;
    FighterArt art = FighterArt::Portrait;
    std::uint64_t fighterPoints = 0, accountPoints = 0;
    int fighterTier = 0;
    // False: a private room shows no points.
    bool points = true;
    // False: the account's points are not known (a friend's card has the main's only), so one line is drawn.
    bool accountKnown = true;
    // The relation chip ("Friends", "Request sent"), none when empty.
    std::string chip;
    ImU32 chipTone = 0;
    // "2210 rated games · 1124 wins · best streak 14"; empty for none.
    std::string record;
    std::vector<player_card::Chip> plays, looking;
    // The shown families and "24 of 37"; left out when `awards.total` is 0.
    player_card::AwardsView awards;
};
// Draws the Panel with its top left at `box.min` across `box.W()`; its height is its content's (`box.max.y` is not
// read): OtherCardHeight gives it for a width, for a caller that sizes what is around it first. With `ground` (the
// default) it stands on a panel ground of its own; without, it is drawn straight onto the panel it is in.
float DrawOtherCard(ImDrawList* d, v2::Box box, const OtherCardView& card, bool ground = true);
float OtherCardHeight(const OtherCardView& card, float width, bool ground = true);
// A player's flag, name and class emblem on a line `row` tall at (x, y), the name at `size` (heading font), cut to `room`.
// Returns `row`.
float DrawNameLine(ImDrawList* d, float x, float y, float room, float row, float size, const std::string& name, CountryCode country, const SkillBadge& badge);
// The player's own card as others see it, from Home's card: the main, its points, the account's and the worn title.
// Without the chips, awards and record: OwnPanel (ProfilePanel.hxx) has them.
OtherCardView OwnAsOthers(const PlayerCardView& card);
// The Panel of a Subject (CardModel.hxx): another player's, or the player's own as others see it.
OtherCardView PanelOf(const player_card::Subject& subject, FighterArt art);
} }
