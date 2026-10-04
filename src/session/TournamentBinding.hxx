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
// (ember_protocol::play::PERMIT_START_SECS). Each permit carries its own
// window, its start_by less its issued_at, both on the bridge's clock; this
// is the one the bridge signs today.
constexpr std::uint64_t PermitStartMs = 120000;
// The longest start window the room takes from a permit.
constexpr std::uint64_t MaximumPermitWindowMs = 600000;
// A game starts at least this long before its permit's window ends, for the
// commits that start it.
constexpr std::uint64_t PermitStartMarginMs = 5000;
// How long two ready fighters wait for the bridge's permit for their next
// game before the start is called off. The permit's own window is enforced
// apart from this hold (RoomAuthority::BeginMatch).
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
