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

// The three public room ops ride the same channel as the tournament ones: they
// are bridge requests the helper answers by request id, and their answers
// reach the runtime beside the assignment list's (PublicRooms.hxx).
struct Command {
	enum class Op : std::uint8_t { None, Refresh, Play, Stop, RoomList, RoomCreate, RoomTicket } op = Op::None;
	std::string bridgeId, matchId;
	// RoomCreate names and sizes the new room; RoomTicket names the room to join.
	std::string roomName, roomId;
	int capacity = 0;
	// The interface's identity for a public room request, increasing, chosen by
	// the PublicRoomsPanel. The runtime carries it to the request's answer, and a
	// newer one supersedes a create or ticket still in flight.
	std::uint64_t request = 0;
	static constexpr std::size_t MaxField = 256;
	bool Valid() const {
		return bridgeId.size() <= MaxField && matchId.size() <= MaxField && roomId.size() <= MaxField &&
			roomName.size() <= 64 && capacity >= 0 && capacity <= 16;
	}
	std::size_t Bytes() const { return bridgeId.size() + matchId.size() + roomName.size() + roomId.size(); }
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
	// A connect link that arrived while the player was not idle at the main
	// menu (in a room, a match or offline play): Ember asks before going on.
	// One that started Ember, or arrived at the main menu, goes on by itself.
	bool confirm = false;
};

// The last public room link a browser opened: the service and room it named.
// The sequence changes with each link. `free` says the player was idle at the
// main menu when it arrived, so asking for the room's ticket is not an
// interruption.
struct OpenedRoomLink {
	std::string bridge, room;
	std::uint64_t sequence = 0;
	bool free = false;
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
	OpenedRoomLink roomLink;
};

} } }
