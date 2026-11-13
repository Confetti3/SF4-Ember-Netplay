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

// One count of points: a fighter's (FP) or the account's (BP). Points 0: nothing
// counted yet.
struct Level {
	// Games + wins: a win is worth 2, a loss 1.
	std::uint64_t points = 0;
	std::uint64_t games = 0, wins = 0;
	// The frame colour, 0 to TierCount - 1, as the bridge decides it, the points
	// at which that tier begins (0 at tier 0), and those at which the next tier
	// begins; next is 0 at the last tier.
	std::uint8_t tier = 0;
	std::uint64_t floor = 0, next = 0;
	// Unix seconds of the first counted game; 0 while there is none.
	std::uint64_t since = 0;
};

// The player's own points, as GET /v1/points/me answers them.
struct Own {
	enum class Kind : std::uint8_t {
		// No Discord account is connected to the Ember ID.
		NoAccount,
		Counted,
		// No answer yet, or the service has no points.
		Unavailable,
	} kind = Kind::Unavailable;
	// Counted: the account's points, on its own tier thresholds. Points 0 while
	// no game is counted.
	Level account;
	// What the milestone awards need beyond the counts (Awards.hxx): the most
	// counted games the account won in a row, and the highest class it ever
	// showed, 1 to SkillBadge::Classes. Both 0 from a bridge that does not
	// send them, and while there is none.
	std::uint32_t bestStreak = 0;
	std::uint8_t peakClass = 0;
	// The most counted games against one opponent, and how many opponents the
	// account has played; 0 from a bridge that does not send them.
	std::uint32_t rival = 0, opponents = 0;
	// Indexed by fighter id, 0 to selection::FighterCount - 1.
	std::array<Level, selection::FighterCount> fighters{};
};

// What a card shows of the account, and of one fighter.
struct CardAccount {
	std::uint64_t points = 0;
	std::uint8_t tier = 0;
};

struct CardFighter {
	std::uint8_t fighter = 0;
	std::uint64_t points = 0;
	std::uint8_t tier = 0;
};

// What a card says of the milestones behind another player's awards: for each
// count, the largest mark the bridge offers that it has reached, never the
// count itself. 0 where the bridge sends none (an older bridge, or a player
// who does not take part in ranking, for the class).
struct CardMarks {
	std::uint64_t games = 0, wins = 0;
	std::uint32_t streak = 0;
	std::uint8_t peakClass = 0;
	std::uint32_t rival = 0, opponents = 0;
};

enum class Relation : std::uint8_t { None, Friend, Sent, Received, Blocked };

// Another player's card from a look-up. A card is public: its points are
// always drawn.
struct Card {
	CardAccount account;
	// The fighters played, ascending; empty when none is.
	std::vector<CardFighter> fighters;
	CardMarks marks;
	Relation relation = Relation::None;
};

// Cards by card token, for the tokens the bridge has a card for.
using CardMap = std::map<std::string, Card>;

} } }
