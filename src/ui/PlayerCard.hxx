#pragma once
// The player's card from their points (docs/design/PROFILE.md): the fighters
// it shows and in what order, how far the next tier is, the view Home's card
// and the Card tab draw from, and the part that draws another player's card.
// The drawing is in PlayerCard.cxx; the choices are here so they are checked
// without a frame (player_card_test.cxx).
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
// One of the Card tab's progress rows: "Ryu · Gold" or "Account · Copper",
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
// The requests waiting in a friends state, 0 until one is read.
int RequestsWaiting(const netplay::friends::View& friends);
// The value of Home's Profile row: what waits in Friends ("2 requests"), empty when nothing does.
std::string ProfileBadge(const netplay::friends::View& friends);
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
// A fighter's picture edge: 2 px inside `box`, in the frame colour of `tier`
// once the fighter has points, plain otherwise.
void DrawFighterEdge(ImDrawList* d, v2::Box box, std::uint64_t points, int tier);
// The Card tab's board at the cursor, `width` wide in a body `height` tall:
// the hero, the player's words and points, progress, facts and strip, whose
// tiles are the `rows` (CardFighterRow), pressed through PressEntry. `changed`
// scrolls a newly focused row into view. Returns the height taken.
float DrawCardTab(const ShellView& view, const PlayerCardView& card, const std::vector<MenuEntry>& rows, MenuNavigation& nav,
    MenuAction& action, bool changed, float height);

// Another player's card (a room member, a friend, the player's own as others
// see it): the picture of their fighter on its tier edge, their flag, name and
// class, the title they wear, `<n> FP` of that fighter and `<n> BP` of the
// account, and a chip for how they stand with the viewer. Nothing else.
struct OtherCardView {
    std::string name;
    CountryCode country;
    SkillBadge badge;
    // The worn title on its tier's plate; none when empty (titles are not served for others yet).
    std::string title;
    v2::Tier titleTier = v2::Tier::None;
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
};
void DrawOtherCard(ImDrawList* d, v2::Box box, const OtherCardView& card);
// A player's flag, name and class emblem on a line `row` tall at (x, y), the name at `size` (heading font), cut to `room`.
// Returns `row`.
float DrawNameLine(ImDrawList* d, float x, float y, float room, float row, float size, const std::string& name, CountryCode country, const SkillBadge& badge);
// The player's own card as others see it: the main, its points and the account's.
OtherCardView OwnAsOthers(const PlayerCardView& card);
} }
