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
	// Redeem opens a match link the player pasted: `handoff` is its one-use code.
	enum class Op : std::uint8_t { None, Refresh, Play, Stop, Redeem } op = Op::None;
	std::string bridgeId, matchId, handoff;
	static constexpr std::size_t MaxField = 256;
	bool Valid() const { return bridgeId.size() <= MaxField && matchId.size() <= MaxField && handoff.size() <= MaxField; }
	std::size_t Bytes() const { return bridgeId.size() + matchId.size() + handoff.size(); }
};

struct Status {
	// The match being played, and how far it got.
	Phase phase = Phase::Idle;
	std::string bridgeId, matchId;
	// Why it finished or failed: a stable code, empty otherwise.
	std::string reason;
	bool waitingForOpponent = false, waitingForPermit = false;
	// The assignment list for `assignmentsBridge`, from its last refresh.
	std::string assignmentsBridge;
	std::vector<Assignment> assignments;
	bool assignmentsLoading = false;
	// The helper's code when the last refresh failed.
	std::string assignmentsError;
	// The last match link opened from a browser or pasted: the service and
	// match it named, or the code of why it could not be opened. The sequence
	// changes with each outcome; a link waiting to be redeemed is pending.
	std::string handoffBridge, handoffMatch, handoffError;
	std::uint64_t handoffSequence = 0;
	bool handoffPending = false;
};

} } }
