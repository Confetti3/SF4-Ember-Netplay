#pragma once
// The counters the milestone awards are reached from (Awards.hxx), in one
// shape wherever they are held: a player's own points answer (levels::Own), a
// card (levels::Card) and the catalog's functions. The wire says six of them,
// under these names in snake case (ember_protocol::awards::AwardCounts):
// games, wins, bestStreak, peakClass, rival and opponents. The decoder
// (session::DecodeAwardCounts) fills those; the fighters played, their highest
// frame and the friends are worked out beside them (awards::CountersOf).
#include <cstdint>

namespace sf4e { namespace netplay { namespace awards {

struct Counters {
	std::uint64_t games = 0, wins = 0;
	std::uint32_t bestStreak = 0, topFighterTier = 0, fightersPlayed = 0, peakClass = 0, friends = 0, rival = 0, opponents = 0;
};

} } }
