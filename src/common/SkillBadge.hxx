#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace sf4e {

// A player's mark in public rooms (docs/design/SKILL_BADGE.md), as the bridge
// signed it into their room ticket: a rating still placing (shown as "?"), a
// class, or Ember's developer mark in place of a class. A class is one of
// USF4's own online classes, 1 (D) to Classes (Grand Master), unsettled after
// a long break, when it may no longer be right. A default SkillBadge is none:
// no Discord account, not taking part, or no ticket with one.
struct SkillBadge {
	static constexpr std::uint8_t Classes = 10;
	enum class Kind : std::uint8_t { None, Placing, Class, Developer };
	Kind kind = Kind::None;
	// A class's band and whether it is settled; 0 and false for every other kind.
	std::uint8_t band = 0;
	bool settled = false;
	static SkillBadge OfClass(std::uint8_t band, bool settled) { return {Kind::Class, band, settled}; }
	static SkillBadge Placing() { return {Kind::Placing, 0, false}; }
	static SkillBadge Developer() { return {Kind::Developer, 0, false}; }
	bool Present() const { return kind != Kind::None; }
	bool HasClass() const { return kind == Kind::Class; }
	bool Valid() const { return kind == Kind::Class ? band >= 1 && band <= Classes : band == 0 && !settled; }
	bool operator==(const SkillBadge& other) const { return kind == other.kind && band == other.band && settled == other.settled; }
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
