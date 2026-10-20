#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace sf4e {

// A player's class in public rooms (docs/design/SKILL_BADGE.md), as the bridge
// signed it into their room ticket: USF4's own online classes, 1 (D) to
// Classes (Grand Master). Unsettled after a long break, when it may no longer
// be right. A default SkillBadge is none: no class yet, or no ticket with one.
struct SkillBadge {
	static constexpr std::uint8_t Classes = 10;
	std::uint8_t band = 0;
	bool settled = false;
	bool Present() const { return band != 0; }
	bool Valid() const { return band <= Classes && (band != 0 || !settled); }
	bool operator==(const SkillBadge& other) const { return band == other.band && settled == other.settled; }
};

// A game a server-owned room saw won, by its two fighters' accounts: what the
// bridge rates classes from. A room keeps its newest MaximumWonGames, which
// the room host reports on every status.
constexpr std::size_t MaximumWonGames = 32;
struct WonGame {
	std::uint8_t table = 0;
	std::uint64_t generation = 0;
	std::string winner, loser;
	bool operator==(const WonGame& other) const {
		return table == other.table && generation == other.generation && winner == other.winner && loser == other.loser;
	}
};

}
