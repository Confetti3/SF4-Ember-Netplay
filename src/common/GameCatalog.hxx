#pragma once

// What Ember's shared code knows of the game's picks: how many fighters it
// has, how far its costume, colour and Ultra numbers go, and its own rules for
// a whole pick and for a stage. The room model, the session protocol, the room
// host and the runtime check picks against these rather than against SF4's
// numbers, so a game with a longer roster (SFV's) or other stages needs no
// change to them.
//
// Each game defines Catalog() once, in its own source (SF4's is
// src/product/Sf4Catalog.cxx), and every executable that checks a pick links
// that definition. A constant table, like ProductIdentity: snapshots and
// preferences built by static initialisers size themselves from it, and a
// binary without a catalog fails to link instead of running with no roster.
//
// C++11 and no game header: the Linux room host compiles it with g++ alone.

#include <cstdint>

namespace sf4e { namespace selection { struct Pick; } }

namespace ember {

struct GameCatalog {
	// Fighters are numbered 0 to fighterCount - 1, the game's own ids.
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
};

const GameCatalog& Catalog();

inline bool ValidFighter(std::int64_t id) { return id >= 0 && id < Catalog().fighterCount; }

} // namespace ember
