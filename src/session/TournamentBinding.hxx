#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <nlohmann/json_fwd.hpp>

// A tournament match bound to a room (spec 13.4, 14): which two Iroh
// endpoints may sit at table 0, in which slot, and how many game wins end
// the set. Each fighter's own helper checks the bridge's signature on it
// before the game sees it; the room only compares and enforces these values.
namespace sf4e { namespace room {

// The table a bound match plays on.
constexpr std::uint8_t TournamentTable = 0;
// How long a game may start after the bridge issues its permit
// (ember_protocol::play::PERMIT_START_SECS).
constexpr std::uint64_t PermitStartMs = 120000;
// How long two ready fighters wait for the bridge's permit for their next
// game before the start is called off. The bridge issues a permit only after
// the room reserved its game, so the permit's start window ends after this
// hold does, and a start that becomes possible at the end of the hold still
// begins inside it, even when a locked-in spectator holds it (static_assert
// in RoomModel.hxx).
constexpr std::uint64_t PermitHoldMs = 105000;
// Longest permit ID the room carries (`per_` and a UUID).
constexpr std::size_t MaximumPermitBytes = 64;

struct TournamentFighter {
	// 64 lowercase hex digits, the member's authenticated room identity.
	std::string endpoint;
	std::string emberId;
	bool operator==(const TournamentFighter& other) const {
		return endpoint == other.endpoint && emberId == other.emberId;
	}
};

struct TournamentBinding {
	std::string matchId;
	std::uint64_t assignmentGeneration = 0;
	std::uint64_t bindingRevision = 0;
	std::uint8_t gamesToWin = 0;
	// By slot: P1, then P2.
	std::array<TournamentFighter, 2> fighters;

	bool Active() const { return !matchId.empty(); }
	// Well formed: a match, two distinct 64-hex endpoints and Ember IDs, and a
	// first-to-1, 2, 3 or 5 set.
	bool Valid() const;
	// The slot `endpoint` holds, or -1.
	int SlotOf(const std::string& endpoint) const;
	// True when `other` names the same match and is not older than this one.
	bool SupersededBy(const TournamentBinding& other) const;
};

// A permit ID as the room accepts it: `per_` and up to 60 more characters
// from the identifier alphabet.
bool ValidPermitId(const std::string& permit);

void to_json(nlohmann::json& json, const TournamentBinding& value);
void from_json(const nlohmann::json& json, TournamentBinding& value);

} }
