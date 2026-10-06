#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// What the interface sees of the bridge's public rooms (docs/design/PUBLIC_ROOMS.md):
// the list from the last refresh, and the answer to the last create or ticket
// request. The helper answers by request id; the runtime decodes each answer
// completely into these types, or reports the answer as invalid.
namespace sf4e { namespace netplay { namespace publicrooms {

// A signed ticket is a few hundred bytes; the bound only keeps a bad answer small.
constexpr std::size_t MaxTicketBytes = 4096;
constexpr std::size_t MaxRooms = 100;

// The most faces a listed room shows, and the longest moderator name.
constexpr std::size_t MaxRoomFighters = 16;
constexpr std::size_t MaxHostNameBytes = 32;

// One row of the bridge's RoomSummary the interface shows. The details after
// `playing` come from a bridge new enough to send them (a list asked for with
// detail=1) and are decoded leniently: a field that is absent or invalid keeps
// its default here, never failing the row.
struct Room {
	std::string id, name, region;
	unsigned members = 0, capacity = 0, playing = 0;
	// When the room opened, in the bridge's seconds; Status::listedAt is the
	// bridge's clock for the same list, so the difference is the room's age.
	std::uint64_t createdAt = 0;
	// The moderator's name, one line of at most MaxHostNameBytes; empty when the
	// room has none or the bridge sent none.
	std::string hostName;
	// Main fighter ids, moderator first and then in join order, -1 for a member
	// with none (the bridge's 255). At most MaxRoomFighters.
	std::vector<int> fighters;
	bool locked = false;
	// True when the bridge sent any of the details above or below.
	bool hasDetails = false;
	// Table 0's set length (0 unlimited, else first to 1 to 10) and rotation (0 winner
	// stays, 1 loser stays, 2 both rotate); -1 when unknown.
	int setFormat = -1, rotation = -1;
};

// A room_list answer: the rooms in the order received and the bridge's clock
// when it listed them (0 when it sent none).
struct RoomList {
	std::vector<Room> rooms;
	std::uint64_t listedAt = 0;
};

// What creating or joining a room returns: the room, the room host's
// invitation, and the signed ticket exactly as received (JSON text), which
// join_public hands to the helper unchanged.
struct Admission {
	Room room;
	std::string invitation, ticket;
};

struct Status {
	// The service the list is for, the rooms it listed in the order received,
	// and whether a refresh is in flight.
	std::string bridge;
	std::vector<Room> rooms;
	bool loading = false;
	// The helper's code when the last refresh failed.
	std::string error;
	// Refreshes finished so far, answered or not.
	std::uint64_t listed = 0;
	// The bridge's clock for the last list that carried one, in the same seconds
	// as Room::createdAt; 0 before one arrives or from a bridge that sends none.
	std::uint64_t listedAt = 0;
	// The identity (Command::request) of the create or ticket request in flight,
	// 0 when none. Only one is: a newer request supersedes it, and its answer is
	// never published.
	std::uint64_t admitting = 0;
	// The identity of the request the answer below belongs to, 0 before the
	// first. `failure` is the helper's code when it was refused; otherwise
	// `admission` is what to join.
	std::uint64_t answered = 0;
	std::string failure;
	Admission admission;
};

} } }
