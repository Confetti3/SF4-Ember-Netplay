#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../common/FighterCatalog.hxx"

namespace sf4e { namespace netplay { namespace levels {

// The level at which a fighter is maxed out.
constexpr int MaxLevel = 60;
// At most this many card look-ups in one request.
constexpr std::size_t MaxLookup = 16;

// One fighter level for the player. Level 0: never played.
struct FighterLevel {
	std::uint8_t level = 0;
	std::uint64_t games = 0, wins = 0;
	// Points (games + wins) at which this level began, and at which the next
	// begins; next is 0 at MaxLevel.
	std::uint64_t floor = 0, next = 0;
	// Unix seconds of the first counted game.
	std::uint64_t since = 0;
};

// The player's own levels, as GET /v1/levels/me answers them.
struct Own {
	enum class Kind : std::uint8_t {
		// No Discord account is connected to the Ember ID.
		NoAccount,
		Counted,
		// No answer yet, or the service has no levels.
		Unavailable,
	} kind = Kind::Unavailable;
	// Counted: whether other players see this player's levels.
	bool shown = false;
	// Indexed by fighter id, 0 to selection::FighterCount - 1.
	std::array<FighterLevel, selection::FighterCount> fighters{};
};

// What a card shows of one fighter.
struct CardFighter {
	std::uint8_t fighter = 0;
	std::uint8_t level = 0;
	std::uint64_t games = 0;
};

enum class Relation : std::uint8_t { None, Friend, Sent, Received, Blocked };

// Another player's card from a look-up.
struct Card {
	// False when the player does not show levels: draw none.
	bool levelsShown = false;
	std::vector<CardFighter> fighters;
	Relation relation = Relation::None;
};

} } }
