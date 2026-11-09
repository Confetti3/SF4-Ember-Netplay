#pragma once

// What Ember's shared code knows of the game's picks: how many fighters it
// has, how far its costume, colour and Ultra numbers go, its own rules for a
// whole pick and for a stage, what the player owns, and the facts the shared
// screens show of a fighter. The room model, the session protocol, the room
// host, the runtime and the shell check and name picks through these rather
// than through SF4's tables, so a game with a longer roster (SFV's) or other
// stages needs no change to them.
//
// Each game defines Catalog() once, in its own source (SF4's is
// src/product/Sf4Catalog.cxx), and every executable that checks a pick links
// that definition, and a binary without a catalog fails to link instead of
// running with no roster. A game may fill its catalog at run time (SFV reads
// its content lists), so nothing is sized from it: per-fighter tables hold
// MaxFighters entries, and only the checks read fighterCount.
//
// C++11 and no game header: the Linux room host compiles it with g++ alone.

#include <cstdint>
#include <vector>

namespace sf4e { namespace selection {
struct Pick;
struct Availability;
struct InstalledContent;
class RandomPool;
} }

namespace ember {

// Fighter ids travel and are saved as one byte (ConfirmedCharaConditions::
// charaID), and 255 stands for none (the room host's NoFighter), so a game
// has at most 255 fighters. Per-fighter tables are this size.
constexpr int MaxFighters = 255;

struct GameCatalog {
	// Fighters are numbered 0 to fighterCount - 1, the game's own ids; at
	// most MaxFighters.
	int fighterCount;
	// Costume and colour numbers a room shows are below these, the game's
	// own and custom ones alike; any other reads as not shared.
	int costumeLimit;
	int colorLimit;
	// Ultra numbers a room shows are below this.
	int ultraChoices;
	// The game's rules for a pick from the network (editionSelect is the
	// room's rule) and for a stage id from the network or a saved choice.
	bool (*validPick)(const sf4e::selection::Pick& pick, bool editionSelect);
	bool (*validStage)(std::int64_t id);
	// The fighters whose faces invite a player on the public rooms setup card.
	int showcaseFighters[4];

	// The stage choice that stands for Random, which resolveStage turns into
	// a stage before it is sent, so it is never valid on the wire; -1 when
	// the game offers no Random stage.
	int randomStage;
	// The player's own content: whether they may play the fighter at all,
	// and whether they own the whole pick, from what the game reports they
	// own (GameState::FighterAvailability) and the custom content installed
	// here (installed may be null). A Ready is refused on either.
	bool (*fighterLocked)(int fighter, const sf4e::selection::Availability& availability);
	bool (*availablePick)(const sf4e::selection::Pick& pick, bool editionSelect,
		const sf4e::selection::Availability& availability, const sf4e::selection::InstalledContent* installed);
	// Makes the pick valid under the edition rule, and owned where
	// availability is given; true when it changed it. A Random fighter's
	// remembered look is fitted to the rolled fighter with it.
	bool (*normalizePick)(sf4e::selection::Pick& pick, bool editionSelect,
		const sf4e::selection::Availability* availability, const sf4e::selection::InstalledContent* installed);
	// The stage P1 sends for a choice: randomStage becomes one of the pool's
	// stages by roll (installed may be null), any other choice a valid stage.
	// `unavailable` holds the stages a seated player at P1's table cannot
	// load (the runtime's unavailableStages, empty where every copy has every
	// stage): a game whose installs can differ rolls Random only over the
	// others, and returns randomStage itself when none is left. SF4's
	// installs never differ, and its catalog ignores it.
	int (*resolveStage)(int choice, std::uint32_t roll, const sf4e::selection::RandomPool& pool,
		const sf4e::selection::InstalledContent* installed, const std::vector<std::int64_t>& unavailable);

	// A fighter's name as every screen shows it; only for a fighter the
	// catalog has (ValidFighter).
	const char* (*fighterName)(int fighter);
	// How many of a fighter's costumes are the game's own, numbered from 0;
	// a costume past them is custom content or not one the game knows.
	int (*costumeCount)(int fighter);
	// Every fighter once, in the order the game's own select screen lists
	// them; fighterCount entries.
	const int* rosterOrder;

	// The table rules the game can play, which the shell offers and no
	// other: round counts and round times in seconds, shortest first, each
	// one a room accepts (room::ValidRoundCount, ValidRoundTime).
	const std::uint8_t* roundCounts;
	int roundCountChoices;
	const std::uint16_t* roundTimes;
	int roundTimeChoices;
	// The game's fighters have editions, so a table's Edition select rule
	// means something.
	bool editions;
	// The game asks its fighters Rematch, Character select or Back to room
	// over its result screen, so a table's Instant rematch rule plays.
	bool rematchInPlace;
};

// A catalog the shared code can use: a roster that fits MaxFighters,
// positive limits, every rule and fact, and a roster order. A game checks its
// catalog with this where it defines or fills it (SF4's at compile time,
// Sf4Catalog.cxx).
constexpr bool Usable(const GameCatalog& catalog) {
	return catalog.fighterCount > 0 && catalog.fighterCount <= MaxFighters && catalog.costumeLimit > 0 &&
		catalog.colorLimit > 0 && catalog.ultraChoices > 0 && catalog.validPick != nullptr && catalog.validStage != nullptr &&
		catalog.fighterLocked != nullptr && catalog.availablePick != nullptr && catalog.normalizePick != nullptr &&
		catalog.resolveStage != nullptr && catalog.fighterName != nullptr && catalog.costumeCount != nullptr &&
		catalog.rosterOrder != nullptr && catalog.roundCounts != nullptr && catalog.roundCountChoices > 0 &&
		catalog.roundTimes != nullptr && catalog.roundTimeChoices > 0;
}

const GameCatalog& Catalog();

inline bool ValidFighter(std::int64_t id) { return id >= 0 && id < Catalog().fighterCount; }

// The fighter's name, or `fallback` for an id the catalog does not have.
inline const char* FighterNameOr(int fighter, const char* fallback) {
	return ValidFighter(fighter) ? Catalog().fighterName(fighter) : fallback;
}

// The game plays this many rounds, or rounds this many seconds long.
inline bool PlayableRoundCount(long long count) {
	for (int i = 0; i < Catalog().roundCountChoices; ++i) if (Catalog().roundCounts[i] == count) return true;
	return false;
}
inline bool PlayableRoundTime(long long seconds) {
	for (int i = 0; i < Catalog().roundTimeChoices; ++i) if (Catalog().roundTimes[i] == seconds) return true;
	return false;
}

} // namespace ember
