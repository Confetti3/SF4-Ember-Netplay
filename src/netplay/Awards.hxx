#pragma once
// The milestone awards (Profile > Awards): what a player earns by playing,
// worked out from counters the bridge attests and never from anything the
// client could make up. This is the one catalog and the pure functions on it,
// so the wall, the notices and another player's worn title are checked by the
// same rules without a frame (awards_test.cxx).
//
// - An award is a family: one icon, one counter and LEVELS. The icon and the
//   plate are drawn in the colour of the highest level reached, so one award
//   climbs from Ash to Ember as the player does. Wearing a family wears its
//   current level, and the title and colour move up by themselves.
// - A level's tier is how hard it is to reach, the same across families: Ash
//   is anyone's first session, Copper a few weeks of play, Silver months, Gold
//   a long-time dedicated player, Ember the top of the ladder or years of play.
// - Every counter only rises (games, wins, the best run, the fighters played,
//   the highest frame, the highest class ever shown, games against one rival,
//   opponents faced), so a level reached is never lost; the friends count is the
//   one that can fall, as the friends themselves can.
// - The player's own counters come from their points answer, and from the
//   friends list for the friends count. Another player's come from their card,
//   which carries the milestones the bridge offers (levels::CardMarks): a
//   family they wear shows at the level their card earns, and not at all at 0.
// - A counter a bridge does not send reads as 0, so its family stays locked: an
//   older bridge with a newer game, and a newer bridge with an older game,
//   both keep working.
#include "Levels.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/SkillBadge.hxx"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sf4e { namespace netplay { namespace awards {

// What a family counts.
enum class Counter : std::uint8_t {
	// Rated games played, and won, by the account.
	Games, Wins,
	// The most games won in a row.
	BestStreak,
	// The highest frame tier any fighter has reached, 0 (Ash) to levels::TierCount - 1.
	TopFighterTier,
	// Fighters with at least one counted game.
	FightersPlayed,
	// The highest class ever shown, 1 to SkillBadge::Classes.
	PeakClass,
	Friends,
	// The most games against one opponent, and how many different opponents.
	Rival, Opponents,
};

struct Counters {
	std::uint64_t games = 0, wins = 0;
	std::uint32_t bestStreak = 0, topFighterTier = 0, fightersPlayed = 0, peakClass = 0, friends = 0, rival = 0, opponents = 0;
};

// One step of a family: reached while the counter is at `need`. `tier` is a
// frame tier, 0 (Ash) to levels::TierCount - 1 (v2::FrameTier gives its
// colours). `title` and `how` are catalog ids; a class level has no title of
// its own, it is "Class" and the class's name as the game shows it (`band`, 2
// to the top; 0 for the others), and its `how` takes that name too.
struct Level {
	std::uint64_t need;
	int tier;
	const char* title;
	const char* how;
	int band;
};

// `icon` is an index into the interface's icon table (v2::Icon); `name` is the
// catalog id of the family's name, which a locked tile shows.
struct Family {
	const char* id;
	int icon;
	Counter counter;
	const char* name;
	const Level* levels;
	std::size_t count;
};

#define SF4E_LEVEL(family, need, tier) {need, tier, "award." family "." #need ".title", "award." family "." #need ".how", 0}
#define SF4E_RANK(band, tier) {band, tier, "", "award.rank.how", band}

inline constexpr Level MatchLevels[] = {SF4E_LEVEL("matches", 1, 0), SF4E_LEVEL("matches", 100, 1), SF4E_LEVEL("matches", 500, 2),
	SF4E_LEVEL("matches", 1000, 3), SF4E_LEVEL("matches", 5000, 4)};
inline constexpr Level WinLevels[] = {SF4E_LEVEL("wins", 1, 0), SF4E_LEVEL("wins", 50, 1), SF4E_LEVEL("wins", 250, 2),
	SF4E_LEVEL("wins", 1000, 3), SF4E_LEVEL("wins", 2500, 4)};
inline constexpr Level StreakLevels[] = {SF4E_LEVEL("streak", 3, 0), SF4E_LEVEL("streak", 5, 1), SF4E_LEVEL("streak", 10, 2),
	SF4E_LEVEL("streak", 15, 3), SF4E_LEVEL("streak", 20, 4)};
// USF4's own online classes from the second (D+) to the top: nobody needs an
// award for the class they begin below. Ratings start at the floor of B (class
// 5), and the classes are fixed rating ranges, so the colours follow the climb
// above it: B and below Ash, B+ Copper, A Silver, A+ and Master Gold, and the
// top class alone Ember.
inline constexpr Level RankLevels[] = {SF4E_RANK(2, 0), SF4E_RANK(3, 0), SF4E_RANK(4, 0), SF4E_RANK(5, 0), SF4E_RANK(6, 1),
	SF4E_RANK(7, 2), SF4E_RANK(8, 3), SF4E_RANK(9, 3), SF4E_RANK(10, 4)};
// The frame tiers are the difficulty already: a level for each above Ash.
inline constexpr Level MasteryLevels[] = {SF4E_LEVEL("mastery", 1, 1), SF4E_LEVEL("mastery", 2, 2), SF4E_LEVEL("mastery", 3, 3),
	SF4E_LEVEL("mastery", 4, 4)};
inline constexpr Level CastLevels[] = {SF4E_LEVEL("cast", 5, 0), SF4E_LEVEL("cast", 15, 1), SF4E_LEVEL("cast", 30, 2),
	{selection::FighterCount, 3, "award.cast.all.title", "award.cast.all.how", 0}};
inline constexpr Level FriendLevels[] = {SF4E_LEVEL("friends", 1, 0), SF4E_LEVEL("friends", 5, 1), SF4E_LEVEL("friends", 15, 2)};
inline constexpr Level RivalLevels[] = {SF4E_LEVEL("rival", 10, 0), SF4E_LEVEL("rival", 50, 1), SF4E_LEVEL("rival", 100, 2),
	SF4E_LEVEL("rival", 250, 3)};
inline constexpr Level OpponentLevels[] = {SF4E_LEVEL("opponents", 10, 0), SF4E_LEVEL("opponents", 50, 1), SF4E_LEVEL("opponents", 200, 2),
	SF4E_LEVEL("opponents", 500, 3)};
#undef SF4E_LEVEL
#undef SF4E_RANK

// The icons are v2::Icon's order: flag, laurel, flame, shield, medal, bracket,
// the pair of people, history, wave; each family has its own.
#define SF4E_FAMILY(id, icon, counter, levels) {id, icon, Counter::counter, "award." id ".name", levels, sizeof(levels) / sizeof(levels[0])}
inline constexpr Family Catalog[] = {
	SF4E_FAMILY("matches", 7, Games, MatchLevels),
	SF4E_FAMILY("wins", 2, Wins, WinLevels),
	SF4E_FAMILY("streak", 0, BestStreak, StreakLevels),
	SF4E_FAMILY("rank", 10, PeakClass, RankLevels),
	SF4E_FAMILY("mastery", 6, TopFighterTier, MasteryLevels),
	SF4E_FAMILY("cast", 9, FightersPlayed, CastLevels),
	SF4E_FAMILY("friends", 8, Friends, FriendLevels),
	SF4E_FAMILY("rival", 1, Rival, RivalLevels),
	SF4E_FAMILY("opponents", 5, Opponents, OpponentLevels),
};
#undef SF4E_FAMILY
constexpr std::size_t CatalogSize = sizeof(Catalog) / sizeof(Catalog[0]);

static_assert(SkillBadge::Classes == 10, "the rank levels are written for ten classes: add one for a new top class");

inline const Family* Find(const std::string& id) {
	for (const auto& family : Catalog) if (id == family.id) return &family;
	return nullptr;
}

// The ids that may appear in a saved preference or on a card: short, lower case
// letters, digits and hyphens. The catalog says which of them are awards.
inline bool IdShaped(const std::string& id) {
	if (id.empty() || id.size() > 24) return false;
	return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

inline std::uint64_t Have(const Counters& counters, Counter counter) {
	switch (counter) {
	case Counter::Games: return counters.games;
	case Counter::Wins: return counters.wins;
	case Counter::BestStreak: return counters.bestStreak;
	case Counter::TopFighterTier: return counters.topFighterTier;
	case Counter::FightersPlayed: return counters.fightersPlayed;
	case Counter::PeakClass: return counters.peakClass;
	case Counter::Friends: return counters.friends;
	case Counter::Rival: return counters.rival;
	case Counter::Opponents: return counters.opponents;
	}
	return 0;
}

// The level the counters reach: 0 for none, else the number of the highest
// level reached (its index + 1).
inline int LevelOf(const Counters& counters, const Family& family) {
	const auto have = Have(counters, family.counter);
	int level = 0;
	for (std::size_t i = 0; i < family.count; ++i) if (have >= family.levels[i].need) level = static_cast<int>(i) + 1;
	return level;
}

inline bool Earned(const Counters& counters, const Family& family) { return LevelOf(counters, family) > 0; }

// The level `level` (1-based) names; null for 0 or past the top.
inline const Level* LevelAt(const Family& family, int level) {
	return level >= 1 && static_cast<std::size_t>(level) <= family.count ? &family.levels[level - 1] : nullptr;
}

// How far the counter is to the next level: have is never above need. `top`
// says there is no next level.
struct Step {
	std::uint64_t have = 0, need = 0;
	bool top = false;
	// 0 to 1.
	float Part() const { return top || !need ? 1.f : static_cast<float>(static_cast<double>(have) / static_cast<double>(need)); }
};
inline Step Next(const Counters& counters, const Family& family) {
	const int level = LevelOf(counters, family);
	if (static_cast<std::size_t>(level) >= family.count) return {Have(counters, family.counter), 0, true};
	const auto need = family.levels[level].need;
	return {(std::min)(Have(counters, family.counter), need), need, false};
}

// Whether the progress reads as "37 of 100": a count of things. A class and a
// frame are steps, not counts, and only say they are reached or not.
inline bool Counts(const Family& family) {
	return family.counter != Counter::PeakClass && family.counter != Counter::TopFighterTier;
}

inline std::size_t LevelTotal() {
	std::size_t total = 0;
	for (const auto& family : Catalog) total += family.count;
	return total;
}
// Levels reached over every family.
inline int LevelsEarned(const Counters& counters) {
	int earned = 0;
	for (const auto& family : Catalog) earned += LevelOf(counters, family);
	return earned;
}

// The player's own counters from their points answer (all 0 unless it is
// counted) and their friends count.
inline Counters CountersOf(const levels::Own& own, std::uint32_t friends) {
	Counters counters;
	counters.friends = friends;
	if (own.kind != levels::Own::Kind::Counted) return counters;
	counters.games = own.account.games;
	counters.wins = own.account.wins;
	counters.bestStreak = own.bestStreak;
	counters.peakClass = own.peakClass;
	counters.rival = own.rival;
	counters.opponents = own.opponents;
	for (const auto& fighter : own.fighters) {
		if (fighter.games == 0) continue;
		++counters.fightersPlayed;
		counters.topFighterTier = (std::max<std::uint32_t>)(counters.topFighterTier, fighter.tier);
	}
	return counters;
}

// Another player's counters from their card: the milestones it carries for
// games, wins, the best run, the class and rivals, the fighters it lists for
// the cast and the frames. Their friends are not on a card, so that family
// reads as locked there.
inline Counters CountersOf(const levels::Card& card) {
	Counters counters;
	counters.games = card.marks.games;
	counters.wins = card.marks.wins;
	counters.bestStreak = card.marks.streak;
	counters.peakClass = card.marks.peakClass;
	counters.rival = card.marks.rival;
	counters.opponents = card.marks.opponents;
	counters.fightersPlayed = static_cast<std::uint32_t>(card.fighters.size());
	for (const auto& fighter : card.fighters)
		counters.topFighterTier = (std::max<std::uint32_t>)(counters.topFighterTier, fighter.tier);
	return counters;
}

// A family worn at the level the counters reach.
struct Worn {
	const Family* family = nullptr;
	int level = 0;
	explicit operator bool() const { return family != nullptr; }
	const Level& Current() const { return family->levels[level - 1]; }
};

// The family a saved or received id names, at the level `counters` reach: none
// for an id that is no award (one that was removed, or made up), or a family at
// level 0. This is the check that keeps a claim nobody earned from showing, on
// the player's own card and on anyone else's.
inline Worn WornOf(const Counters& counters, const std::string& id) {
	if (!IdShaped(id)) return {};
	const Family* family = Find(id);
	if (!family) return {};
	const int level = LevelOf(counters, *family);
	return level > 0 ? Worn{family, level} : Worn{};
}

// One level a counters answer newly reached.
struct Gained {
	const Family* family;
	int level;
};

// Which levels a counters answer reached that the one before did not, for a
// notice that says each once, family by family and low to high. The first
// answer of a run only seeds what is reached, so an update that adds awards
// does not announce them all, and an answer that is not counted changes
// nothing. A level reached stays seen even if its counter later falls. The
// friends family is seeded silently too: the friends list arrives on its own
// after the points, and the friends already made would read as new at every start.
class Watch {
public:
	// Counted: `counters` is a counted answer's (otherwise nothing is learned).
	std::vector<Gained> Update(const Counters& counters, bool counted) {
		std::vector<Gained> gained;
		if (!counted) return gained;
		for (const auto& family : Catalog) {
			const int level = LevelOf(counters, family);
			int& seen = seen_[family.id];
			if (level > seen) {
				if (seeded_ && family.counter != Counter::Friends)
					for (int reached = seen + 1; reached <= level; ++reached) gained.push_back({&family, reached});
				seen = level;
			}
		}
		seeded_ = true;
		return gained;
	}

private:
	std::map<std::string, int> seen_;
	bool seeded_ = false;
};

} } }
