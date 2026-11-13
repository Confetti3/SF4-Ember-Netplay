#pragma once
// What a player's card shows and in what sizes (docs/design/PROFILE.md): the
// view models of the Full card, the Panel and the set rows, and the choices
// behind them (which chips, which award families, which fighters, which words),
// all made by plain functions that need no frame (player_card_test.cxx). The
// drawing is CardParts.cxx and PlayerCard.cxx, which only draw what is here;
// the same code draws the player's own card and another's, the caller saying
// whose it is by the data it gives (Subject).
#include "GameMenu.hxx"
#include "FighterArt.hxx"
#include "MenuRows.hxx"
#include "../netplay/Answered.hxx"
#include "../netplay/Awards.hxx"
#include "../netplay/Levels.hxx"
#include "../netplay/Profiles.hxx"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sf4e { namespace ui { namespace player_card {

// ---- Sizes ----------------------------------------------------------------

// The smallest text on any card, in px at Interface size 1.00x.
constexpr float TextFloor = 13.f;

// Full (1920x1080), compact (a body under 620 logical px tall: 1280x720) and
// narrow (under 900 logical px wide: one scrolling column).
enum class Size : std::uint8_t { Wide, Compact, Narrow };
inline Size SizeOf(bool narrow, float logicalBodyHeight) { return narrow ? Size::Narrow : logicalBodyHeight < 620.f ? Size::Compact : Size::Wide; }

// Every measure of the Full card and its parts for one size, in logical px
// (the drawing multiplies by Scale()). The text sizes are named `...Text`;
// none is under TextFloor. A measure of 0 is shared out by the drawing (the
// narrow column's tiles take the width).
struct Metrics {
    float hero, gutter, middle, right;
    float titleLine, titleGap, nameRow, nameText, aboutRow, aboutText, emblem;
    float chipRow, groupText, groupGap;
    float digits, meter, wordsText, pointsGap;
    float recordKeyText, recordValueText;
    float heading, tileWidth, tileGap, tileNameText, tileGamesText;
    float familyWidth, familyHeight, familyGap, plate, icon, pip, pipGap, familyTitleText;
    float rowHeight, rowGap, rowPicture, rowNameText, rowLineText, linkRow;
    float button, buttonGap, buttonGapNext;
    float heroWordText, heroNameText, noteText;
    int setRows;
    // The smallest text the card draws at this size.
    float SmallestText() const;
};
const Metrics& MetricsOf(Size size);

// ---- Chips ----------------------------------------------------------------

struct Chip {
    enum class Kind : std::uint8_t { Device, Link, Region, Looking } kind = Kind::Device;
    std::string text;
};
// Plays: the device, the connection and the region, in that order, those the style has.
std::vector<Chip> PlaysChips(const netplay::profiles::Style& style);
// Looking for: up to two choices, in the order a screen lists them.
std::vector<Chip> LookingChips(const netplay::profiles::Style& style);
// Whether the chips fit the room the drawing has.
using Fits = std::function<bool(const std::vector<Chip>&)>;
// The Plays chips that fit: Region goes first, then Connection; the device stays.
std::vector<Chip> GiveWay(std::vector<Chip> plays, const Fits& fits);

// ---- Awards ---------------------------------------------------------------

// A level's title as the card says it: "World Warrior", or "Class A+".
std::string LevelTitle(const netplay::awards::Level& level);
// One family at the level a card's counters earn, never at one claimed.
struct FamilyTile {
    const netplay::awards::Family* family = nullptr;
    int level = 0;
    v2::Tier tier = v2::Tier::None;
    std::string title;
};
// Up to `most` families to show: the ones chosen (`pins`) that have a level,
// then, to fill, the others by the tier of their level (highest first), how far
// along their levels they are, and the catalog's order. Families at level 0 are
// never shown.
std::vector<FamilyTile> ShownFamilies(const netplay::awards::Counters& counters, const std::vector<std::string>& pins, std::size_t most = netplay::profiles::MaxPins);
// The worn title and icon at the level the counters earn; each absent when its
// family is unknown or has no level.
struct WornView {
    std::string title, family;
    v2::Tier titleTier = v2::Tier::None;
    int icon = -1;
    v2::Tier iconTier = v2::Tier::None;
    bool Any() const { return !title.empty() || icon >= 0; }
};
WornView WornOnCard(const netplay::awards::Counters& counters, const netplay::profiles::Worn& worn);
// The showcase: its families, the levels reached of all levels ("28 of 40").
// `friends` says whether the friends family counts (the player's own card; the
// count of someone else's is never served, so their total leaves it out).
struct AwardsView {
    std::vector<FamilyTile> tiles;
    int earned = 0, total = 0;
};
AwardsView AwardsOf(const netplay::awards::Counters& counters, const std::vector<std::string>& pins, bool friends);

// ---- Points and record ----------------------------------------------------

// One points line: its number, and, on the player's own card, the meter and the
// words after it ("Gold · 514 points to Ember").
struct PointsLine {
    std::uint64_t points = 0;
    bool meter = false;
    float part = 0;
    std::string words;
};
struct PointsView {
    bool shown = false;
    PointsLine hero, account;
};
// The meter's part and words for a count of points: "Gold · 514 points to
// Ember", "Gold · Top frame", or "Not played yet" for a fighter at 0.
PointsLine MeterLine(const netplay::levels::Level& level, bool account);
// The four record cells: Rated games, Win rate (`winRate`) or Wins, Best
// streak, Fighters played ("12 of 44").
std::vector<v2::TagView::Fact> RecordCells(const netplay::awards::Counters& counters, bool winRate);
// "2840 rated games · 1164 wins · best streak 12".
std::string RecordLine(const netplay::awards::Counters& counters);

// ---- Parts of the Full card ------------------------------------------------

struct StripTile {
    int fighter = -1;
    std::uint64_t points = 0;
    int tier = 0;
    bool isMain = false;
    // "1324 games", or empty where games are not shown (another's card).
    std::string games;
    std::string row;
};
// The fighters of the strip: the main first (at 0 points when unplayed), then
// the highest others, up to `most` in all. `byPoints` lists every fighter with
// points, highest first.
std::vector<int> TopFighters(const std::vector<int>& byPoints, int main, std::size_t most = 3);

// What stands where the own card names the most played opponent.
struct Rival {
    enum class Kind : std::uint8_t { None, MostPlayed, MostPlayedNone, Versus } kind = Kind::None;
    // MostPlayed: the player, and the line "41 – 33 · 74 games".
    netplay::levels::MostPlayed player;
    std::string line;
    // Versus: the cells "You and Player 2", "Last played", "Friends".
    std::vector<v2::TagView::Fact> cells;
};

// A recent set as a row says it.
struct SetRow {
    std::string row;
    // This player's fighter and the opponent's: -1 for none known.
    int mine = -1, theirs = -1;
    std::string opponent;
    CountryCode country;
    SkillBadge badge;
    // "3 – 1 · Online · 2 d ago".
    std::string line;
};
struct SetsView {
    enum class State : std::uint8_t { Reading, Rows, None, Unavailable, Unreachable } state = State::Reading;
    std::vector<SetRow> rows;
    // False: the part holds no way to the Sets tab (the service keeps no archive).
    bool allSets = true;
    // False: the card has no recent sets part at all (a player the viewer blocked).
    bool shown = true;
};

// A button at the hero's foot: the row it presses, the pad button's glyph (null
// for none, as the Primary button has), its kind.
struct HeroButton {
    std::string row;
    const char* glyph = nullptr;
    v2::ButtonKind kind = v2::ButtonKind::Secondary;
};

enum class PresenceMark : std::uint8_t { None, InRoom, Online, Offline };

// Everything a card reads of a player, whoever they are. The player's own card
// and another's are made from this by their own adapters (OwnSubject,
// CardSubject); what differs between them is data, not a flag the drawing asks.
struct Subject {
    CardLevels levels = CardLevels::Counted;
    std::string name;
    CountryCode country;
    SkillBadge badge;
    // Unix seconds, the first of the month they came to Ember; 0 when not known.
    std::uint64_t since = 0;
    // Their main, and the fighter the card shows (the one they play in a room), -1 for none.
    int main = -1, fighter = -1;
    bool playing = false;
    netplay::profiles::Worn worn;
    std::vector<std::string> pins;
    netplay::profiles::Style style;
    netplay::awards::Counters counters;
    // The friends count is the player's own: it counts in the awards' total.
    bool friendsCounted = false;
    // The player's own card: meters on the points lines, games under a tile and a win rate.
    bool detailed = false;
    // False for a card from a service without profiles: no chips, record, awards or since.
    bool profiled = true;
    // The shown fighter's points and the account's, floors and next included for the meters.
    netplay::levels::Level hero, account;
    // Every fighter with points, highest first.
    std::vector<CardFighter> played;
    // Friends only: a line under the about line.
    PresenceMark presenceMark = PresenceMark::None;
    std::string presence;
    // The most played opponent (own) and its state.
    bool hasMostPlayed = false;
    netplay::levels::MostPlayed mostPlayed;
    // The record between the viewer and this player (another's): the relation, since when they are friends, now.
    bool versus = false;
    bool hasRecord = false;
    netplay::profiles::Versus record;
    netplay::levels::Relation relation = netplay::levels::Relation::None;
    std::uint64_t friendSince = 0, now = 0;
    SetsView sets;
    std::vector<HeroButton> buttons;
    // Plain words in the place of the points lines and the record, for a card there is nothing to draw of (reading, no card,
    // a blocked player, unreachable): one paragraph each, as given.
    std::vector<std::string> notes;
};

struct FullCardView {
    Size size = Size::Wide;
    CardLevels levels = CardLevels::Counted;
    std::string name;
    CountryCode country;
    SkillBadge badge;
    std::string about;
    PresenceMark presenceMark = PresenceMark::None;
    std::string presence;
    WornView worn;
    int hero = -1;
    std::string heroWord, heroName;
    std::uint64_t heroPoints = 0;
    int heroTier = 0;
    std::vector<Chip> plays, looking;
    PointsView points;
    std::vector<v2::TagView::Fact> record;
    std::vector<StripTile> strip;
    AwardsView awards;
    Rival rival;
    SetsView sets;
    std::vector<HeroButton> buttons;
    // The words for a points state with nothing to show, or null.
    const char* levelWords = nullptr;
    // The subject's own words, which stand in place of levelWords when there are any.
    std::vector<std::string> notes;
    // How many set rows the size shows.
    int setRows = 5;
};
FullCardView FullOf(const Subject& subject, Size size);

// The player's own subject from the points answer and what the card carries.
struct OwnInputs {
    const netplay::Answered<netplay::levels::Own>* levels = nullptr;
    std::string name;
    CountryCode country;
    SkillBadge badge;
    std::uint64_t since = 0;
    int main = -1;
    netplay::profiles::Worn worn;
    std::vector<std::string> pins;
    netplay::profiles::Style style;
    std::uint32_t friends = 0;
};
Subject OwnSubject(const OwnInputs& inputs);
// Another player's subject from their card. `known` is what the opener knew
// (a name before the card arrives), `fighter` the one they play now (-1: their main).
Subject CardSubject(const netplay::levels::Card& card, const netplay::profiles::Known& known, int fighter, bool playing, bool profiled);

// The rows the card's own parts are (strip tiles, award tiles, Most played, the
// recent sets, All sets), after the caller's own: Choose a main, the hero's
// buttons.
constexpr const char* AwardRowPrefix = "card-award:";
constexpr const char* MostPlayedRow = "card-most-played";
constexpr const char* AllSetsRow = "card-all-sets";
inline std::string CardAwardRow(const std::string& family) { return AwardRowPrefix + family; }
std::vector<MenuEntry> CardRows(const FullCardView& view);


// ---- Small words ----------------------------------------------------------

// "2 h ago", "1 d ago", or the date once it is older than three days; empty for 0.
std::string WhenWords(std::uint64_t now, std::uint64_t at);

} } }
