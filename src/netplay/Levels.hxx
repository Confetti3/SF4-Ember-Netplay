#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "../common/FighterCatalog.hxx"

namespace sf4e { namespace netplay { namespace levels {

// The frame tiers, Ash to Ember (v2::TierColor): tier 0 to TierCount - 1.
constexpr int TierCount = 5;
// At most this many card look-ups in one request.
constexpr std::size_t MaxLookup = 16;

// One fighter's points for the player. Points 0: never played.
struct FighterLevel {
	// Games + wins: a win is worth 2, a loss 1.
	std::uint64_t points = 0;
	std::uint64_t games = 0, wins = 0;
	// The frame colour, 0 to TierCount - 1, as the bridge decides it, the points
	// at which that tier begins (0 at tier 0), and those at which the next tier
	// begins; next is 0 at the last tier.
	std::uint8_t tier = 0;
	std::uint64_t floor = 0, next = 0;
	// Unix seconds of the first counted game.
	std::uint64_t since = 0;
};

// The player's own points, as GET /v1/levels/me answers them.
struct Own {
	enum class Kind : std::uint8_t {
		// No Discord account is connected to the Ember ID.
		NoAccount,
		Counted,
		// No answer yet, or the service has no points.
		Unavailable,
	} kind = Kind::Unavailable;
	// Counted: whether other players see this player's points.
	bool shown = false;
	// Indexed by fighter id, 0 to selection::FighterCount - 1.
	std::array<FighterLevel, selection::FighterCount> fighters{};
};

// What a card shows of one fighter.
struct CardFighter {
	std::uint8_t fighter = 0;
	std::uint64_t points = 0;
	std::uint8_t tier = 0;
};

enum class Relation : std::uint8_t { None, Friend, Sent, Received, Blocked };

// Another player's card from a look-up.
struct Card {
	// False when the player does not show points: draw none.
	bool levelsShown = false;
	std::vector<CardFighter> fighters;
	Relation relation = Relation::None;
};

// Cards by card token, for the tokens the bridge has a card for.
using CardMap = std::map<std::string, Card>;

} } }
