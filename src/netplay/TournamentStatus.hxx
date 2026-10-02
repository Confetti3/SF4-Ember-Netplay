#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "TournamentPlay.hxx"

// What the interface sees of tournament play and asks of it: the player's
// assigned matches from the last refresh, the match being played, and the
// three things a player can do (refresh the list, play a match, stop).
namespace sf4e { namespace netplay { namespace tournament {

// One of the player's assigned matches, for the assignment list.
struct Assignment {
	std::string matchId, provider, state, roundLabel, opponentFingerprint, profile;
	int slot = -1;
	int gamesToWin = 0;
	std::array<unsigned, 2> wins = {};
	// Played through Ember (ember-room-v1) rather than entered by an organizer.
	bool PlayedInEmber() const { return profile == "ember-room-v1"; }
	bool Finished() const { return state == "completed" || state == "cancelled" || state == "failed"; }
};

struct Command {
	enum class Op : std::uint8_t { None, Refresh, Play, Stop } op = Op::None;
	std::string bridgeId, matchId;
	static constexpr std::size_t MaxField = 256;
	bool Valid() const { return bridgeId.size() <= MaxField && matchId.size() <= MaxField; }
	std::size_t Bytes() const { return bridgeId.size() + matchId.size(); }
};

// The assignment list for `bridge`, from its last refresh.
struct AssignmentList {
	std::string bridge;
	std::vector<Assignment> items;
	bool loading = false;
	// The helper's code when the last refresh failed.
	std::string error;
	// Refreshes finished so far, answered or not, so a reader can tell a newer one.
	std::uint64_t finished = 0;
};

// The last match link a browser opened: the service and match it named, or
// the last Discord connect link: the service alone. The sequence changes
// with each link.
struct OpenedLink {
	std::string bridge, match;
	std::uint64_t sequence = 0;
	// When it arrived (GetTickCount64), for its age.
	std::uint64_t at = 0;
};

struct Status {
	// The match being played, and how far it got.
	Phase phase = Phase::Idle;
	std::string bridgeId, matchId;
	// Why it finished or failed: a stable code, empty otherwise.
	std::string reason;
	bool waitingForOpponent = false, waitingForPermit = false;
	AssignmentList list;
	OpenedLink link, connect;
	// How long ago the connect link arrived, in milliseconds.
	std::uint64_t connectAgeMs = 0;
};

} } }
