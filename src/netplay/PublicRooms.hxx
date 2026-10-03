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

// One row of the bridge's RoomSummary the interface shows.
struct Room {
	std::string id, name, region;
	unsigned members = 0, capacity = 0, playing = 0;
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
