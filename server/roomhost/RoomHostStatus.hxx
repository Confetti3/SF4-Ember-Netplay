#pragma once

// The room host's status line for the supervisor (server/ember-rooms/src/
// protocol.rs). Plain inputs and no helper or transport, so a test can build the
// largest line, and the details of a room model, without either.
#include "../../src/session/RoomModel.hxx"
#include <cstddef>
#include <string>
#include <vector>

namespace sf4e { namespace roomhost {

// The status line's limits, which the room host, the supervisor and the bridge
// each enforce and which must agree (server/roomhost/README.md, "Status line
// limits"). The supervisor kills a child whose line passes SupervisorLineBytes
// (MAX_LINE_BYTES in server/ember-rooms/src/protocol.rs); the host ends the
// room itself well before that, at MaximumStatusLineBytes. A room holds at most
// room::MaximumKickedAccounts (512) bans, each an Ember ID of 57 bytes, and
// MaximumWonGames (32) won games of two, so with an invitation of 4096 bytes
// and the details a full line is about 43 KB.
constexpr std::size_t SupervisorLineBytes = 128 * 1024;
constexpr std::size_t MaximumStatusLineBytes = 60000;
static_assert(MaximumStatusLineBytes < SupervisorLineBytes, "the host must end the room before the supervisor kills it");
static_assert(room::MaximumKickedAccounts == 512, "MAX_ROOM_BANS in ember-rooms, ember-protocol and the helper");
static_assert(MaximumWonGames == 32, "MAX_GAMES_BYTES in ember-rooms and the bridge allow for 32 games");
// The supervisor passes on a details object of at most SupervisorDetailsBytes
// and a games array of at most SupervisorGamesBytes, as written, and drops a
// larger one (MAX_DETAILS_BYTES and MAX_GAMES_BYTES in protocol.rs).
constexpr std::size_t SupervisorDetailsBytes = 2048, SupervisorGamesBytes = 6144;

// What the bridge lists beside a room (the status line's "details" object).
// Everything is copied from the room model, so a change re-emits the line.
struct RoomDetails {
	std::string name;
	// The moderator's display name (HostDisplayName); empty when the room has none.
	std::string hostName;
	unsigned capacity = 0;
	bool locked = false;
	// Moderator first, then the rest in join order; each a main fighter id or 255
	// for none, at most MaximumDetailFighters.
	std::vector<int> fighters;
	// Table 0's rules. setFormat is the first-to count (0 unlimited, else 1 to 10).
	// rotation is room::RotationMode's value: 0 winner stays, 1 loser stays,
	// 2 both rotate.
	int setFormat = 0, rotation = 0;
	// Each fighter plays at their own input delay (room::DelayPolicy::Own). Never
	// changes during the room's life.
	bool ownDelay = false;
	bool operator==(const RoomDetails& other) const {
		return name == other.name && hostName == other.hostName && capacity == other.capacity &&
			locked == other.locked && fighters == other.fighters && setFormat == other.setFormat &&
			rotation == other.rotation && ownDelay == other.ownDelay;
	}
};
constexpr int NoFighter = 255;
constexpr std::size_t MaximumDetailFighters = 16;
constexpr std::size_t MaximumHostNameBytes = 32;

// A member name as the bridge accepts a host name: valid UTF-8 with control
// characters replaced by spaces, surrounding whitespace trimmed, and at most 32
// bytes, cut on a character boundary.
std::string HostDisplayName(const std::string& name);

// The details of a room model's snapshot.
RoomDetails DetailsOf(const room::Snapshot& snapshot);

// Everything one status line says. The host emits a line when this changes.
struct Status {
	std::size_t members = 0, tablesPlaying = 0;
	std::string invitation;
	// Committed bans and won games only (SessionServer::CommittedServerOwned).
	std::vector<std::string> banned;
	RoomDetails details;
	std::vector<WonGame> games;
	bool operator==(const Status& other) const {
		return members == other.members && tablesPlaying == other.tablesPlaying && invitation == other.invitation &&
			banned == other.banned && details == other.details && games == other.games;
	}
};

// {"type":"status","members":n,"tables_playing":n,"invitation":s,"banned":[...],
//  "details":{"name":s,"capacity":n,"locked":b,"host_name":s (only with a
//  moderator),"fighters":[n...],"set_format":n,"rotation":n,"own_delay":true (only
//  for a room whose fighters each play at their own input delay)},
//  "games":[{"table":n,"generation":n,"winner":s,"loser":s}...] (only once the
//  room has a won game, oldest first)}
std::string StatusLine(const Status& status);

} }
