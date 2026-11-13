#pragma once
// The milestone awards (Profile > Awards): what a player earns by playing,
// worked out from counters the bridge attests and never from anything the
// client could make up. This is the one catalog and the pure functions on it,
// so the wall, the notices and another player's worn title are checked by the
// same rules without a frame (awards_test.cxx).
//
// - An award is earned while one counter reaches its need. Every counter only
//   rises (games, wins, the best run, the fighters played, the highest frame,
//   the highest class ever shown), so an earned award is never taken away; the
//   friends count is the one that can fall, as the friends themselves can.
// - The player's own counters come from their points answer, and from the
//   friends list for the friends count. Another player's come from their card,
//   which carries the milestones the bridge offers (levels::CardMarks): an
//   award they wear shows only when their card earns it.
// - A counter a bridge does not send reads as 0, so its awards show as locked:
//   an older bridge with a newer game, and a newer bridge with an older game,
//   both keep working.
#include "Levels.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/SkillBadge.hxx"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace sf4e { namespace netplay { namespace awards {

// The groups the wall shows under a small heading, in this order.
enum class Family : std::uint8_t { Games, Wins, Streak, Class, Fighters, Cast, Friends };
constexpr int FamilyCount = 7;

// What an award counts.
enum class Counter : std::uint8_t {
	// Rated games played, and won, by the account.
	Games, Wins,
	// The most games won in a row.
	BestStreak,
	// Fighters with at least one counted game.
	FightersPlayed,
	// The highest frame tier any fighter has reached, 0 (Ash) to levels::TierCount - 1.
	TopFighterTier,
	// The highest class ever shown, 1 to SkillBadge::Classes.
	PeakClass,
	Friends,
};

struct Counters {
	std::uint64_t games = 0, wins = 0;
	std::uint32_t bestStreak = 0, fightersPlayed = 0, topFighterTier = 0, peakClass = 0, friends = 0;
};

// `tier` is a frame tier, 0 (Ash) to levels::TierCount - 1 (v2::FrameTier gives
// its colours). `icon` is an index into the interface's icon table (v2::Icon):
// one for each family, the tier colour telling its steps apart. `title` and
// `how` are catalog ids; a class award has no title of its own, it is "Class"
// and the class's name as the game shows it (`band`, 2 to the top; 0 for the
// others), and its `how` takes that name too.
struct Award {
	const char* id;
	Family family;
	Counter counter;
	std::uint64_t need;
	int tier;
	int icon;
	const char* title;
	const char* how;
	int band;
};

// The icons (v2::Icon's order) each family wears.
constexpr int IconGames = 7, IconWins = 2, IconStreak = 0, IconClass = 10, IconFighters = 6, IconCast = 9, IconFriends = 8;

#define SF4E_AWARD(id, family, counter, need, tier, icon, band) \
	{id, Family::family, Counter::counter, need, tier, icon, "award." id ".title", "award." id ".how", band}
// Lowest need first inside a family. The classes are USF4's own online classes
// from the second (D+) to the top: nobody needs an award for the class they
// start in, and the tiers rise with the band from Ash to Ember.
inline constexpr Award Catalog[] = {
	SF4E_AWARD("games-1", Games, Games, 1, 0, IconGames, 0),
	SF4E_AWARD("games-10", Games, Games, 10, 0, IconGames, 0),
	SF4E_AWARD("games-100", Games, Games, 100, 1, IconGames, 0),
	SF4E_AWARD("games-500", Games, Games, 500, 2, IconGames, 0),
	SF4E_AWARD("games-1000", Games, Games, 1000, 3, IconGames, 0),
	SF4E_AWARD("games-5000", Games, Games, 5000, 4, IconGames, 0),
	SF4E_AWARD("wins-1", Wins, Wins, 1, 0, IconWins, 0),
	SF4E_AWARD("wins-50", Wins, Wins, 50, 1, IconWins, 0),
	SF4E_AWARD("wins-250", Wins, Wins, 250, 2, IconWins, 0),
	SF4E_AWARD("wins-1000", Wins, Wins, 1000, 3, IconWins, 0),
	SF4E_AWARD("wins-2500", Wins, Wins, 2500, 4, IconWins, 0),
	SF4E_AWARD("streak-3", Streak, BestStreak, 3, 0, IconStreak, 0),
	SF4E_AWARD("streak-5", Streak, BestStreak, 5, 1, IconStreak, 0),
	SF4E_AWARD("streak-10", Streak, BestStreak, 10, 3, IconStreak, 0),
	SF4E_AWARD("streak-20", Streak, BestStreak, 20, 4, IconStreak, 0),
	{"class-2", Family::Class, Counter::PeakClass, 2, 0, IconClass, "", "award.class.how", 2},
	{"class-3", Family::Class, Counter::PeakClass, 3, 0, IconClass, "", "award.class.how", 3},
	{"class-4", Family::Class, Counter::PeakClass, 4, 1, IconClass, "", "award.class.how", 4},
	{"class-5", Family::Class, Counter::PeakClass, 5, 1, IconClass, "", "award.class.how", 5},
	{"class-6", Family::Class, Counter::PeakClass, 6, 2, IconClass, "", "award.class.how", 6},
	{"class-7", Family::Class, Counter::PeakClass, 7, 2, IconClass, "", "award.class.how", 7},
	{"class-8", Family::Class, Counter::PeakClass, 8, 3, IconClass, "", "award.class.how", 8},
	{"class-9", Family::Class, Counter::PeakClass, 9, 3, IconClass, "", "award.class.how", 9},
	{"class-10", Family::Class, Counter::PeakClass, 10, 4, IconClass, "", "award.class.how", 10},
	SF4E_AWARD("fighter-copper", Fighters, TopFighterTier, 1, 1, IconFighters, 0),
	SF4E_AWARD("fighter-gold", Fighters, TopFighterTier, 3, 3, IconFighters, 0),
	SF4E_AWARD("fighter-ember", Fighters, TopFighterTier, 4, 4, IconFighters, 0),
	SF4E_AWARD("cast-5", Cast, FightersPlayed, 5, 0, IconCast, 0),
	SF4E_AWARD("cast-15", Cast, FightersPlayed, 15, 2, IconCast, 0),
	SF4E_AWARD("cast-all", Cast, FightersPlayed, selection::FighterCount, 4, IconCast, 0),
	SF4E_AWARD("friends-1", Friends, Friends, 1, 0, IconFriends, 0),
	SF4E_AWARD("friends-10", Friends, Friends, 10, 1, IconFriends, 0),
};
#undef SF4E_AWARD
constexpr std::size_t CatalogSize = sizeof(Catalog) / sizeof(Catalog[0]);

static_assert(SkillBadge::Classes == 10, "the class awards are written for ten classes: add one for a new top class");

inline const Award* Find(const std::string& id) {
	for (const auto& award : Catalog) if (id == award.id) return &award;
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
	case Counter::FightersPlayed: return counters.fightersPlayed;
	case Counter::TopFighterTier: return counters.topFighterTier;
	case Counter::PeakClass: return counters.peakClass;
	case Counter::Friends: return counters.friends;
	}
	return 0;
}

inline bool Earned(const Counters& counters, const Award& award) { return Have(counters, award.counter) >= award.need; }

// How far the counter is to the award's need: have never above need.
struct Step {
	std::uint64_t have = 0, need = 0;
	// 0 to 1.
	float Part() const { return need ? static_cast<float>(static_cast<double>(have) / static_cast<double>(need)) : 1.f; }
};
inline Step Progress(const Counters& counters, const Award& award) {
	return {(std::min)(Have(counters, award.counter), award.need), award.need};
}

// Whether the award says its progress as "37 of 100": a count of things. A
// class and a frame are steps, not counts, and are only earned or not.
inline bool Counts(const Award& award) {
	return award.counter != Counter::PeakClass && award.counter != Counter::TopFighterTier;
}

inline int EarnedCount(const Counters& counters) {
	int earned = 0;
	for (const auto& award : Catalog) earned += Earned(counters, award);
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
	for (const auto& fighter : own.fighters) {
		if (fighter.games == 0) continue;
		++counters.fightersPlayed;
		counters.topFighterTier = (std::max<std::uint32_t>)(counters.topFighterTier, fighter.tier);
	}
	return counters;
}

// Another player's counters from their card: the milestones it carries for
// games, wins, the best run and the class, the fighters it lists for the
// cast and the frames. Their friends are not on a card, so those awards read
// as locked there.
inline Counters CountersOf(const levels::Card& card) {
	Counters counters;
	counters.games = card.marks.games;
	counters.wins = card.marks.wins;
	counters.bestStreak = card.marks.streak;
	counters.peakClass = card.marks.peakClass;
	counters.fightersPlayed = static_cast<std::uint32_t>(card.fighters.size());
	for (const auto& fighter : card.fighters)
		counters.topFighterTier = (std::max<std::uint32_t>)(counters.topFighterTier, fighter.tier);
	return counters;
}

// The award a saved or received id names while `counters` earn it: none for an
// id that is no award (one that was removed, or made up) or not earned yet.
// This is the check that keeps a claim nobody earned from showing, on the
// player's own card and on anyone else's.
inline const Award* Worn(const Counters& counters, const std::string& id) {
	if (!IdShaped(id)) return nullptr;
	const Award* award = Find(id);
	return award && Earned(counters, *award) ? award : nullptr;
}

// Which awards a counters answer earned that the one before did not, for a
// notice that says each once. The first answer of a run only seeds what is
// earned, so an update that adds awards does not announce thirty of them, and
// an answer that is not counted (no account, no service) changes nothing. An
// award earned stays seen even if its counter later falls. Friends awards are
// seeded silently too: the friends list arrives on its own after the points,
// and a friend already made would read as a new award at every start.
class Watch {
public:
	// Counted: `counters` is a counted answer's (otherwise nothing is learned).
	std::vector<const Award*> Update(const Counters& counters, bool counted) {
		std::vector<const Award*> fresh;
		if (!counted) return fresh;
		for (const auto& award : Catalog) {
			if (!Earned(counters, award)) continue;
			const bool added = seen_.insert(award.id).second;
			if (added && seeded_ && award.counter != Counter::Friends) fresh.push_back(&award);
		}
		seeded_ = true;
		return fresh;
	}

private:
	std::set<std::string> seen_;
	bool seeded_ = false;
};

} } }
