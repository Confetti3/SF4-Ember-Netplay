#pragma once
// What the interface sees of the player's friends (docs/design/FRIENDS.md,
// "Client"): the bridge's whole FriendsState, decoded into these types by
// session/SocialAnswers.hxx and replaced whole on every answer, as the desk
// does. The state is bounded by the bridge's own limits (the Max constants),
// which the decoder enforces, so a reader never meets a larger one.
//
// Plain types only (C++14: the room model's decoders build them).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../common/Country.hxx"
#include "../common/SkillBadge.hxx"

namespace sf4e { namespace netplay { namespace friends {

// The bridge's limits (ember_protocol::friends).
constexpr std::size_t MaxFriends = 200, MaxIncoming = 50, MaxInvites = 20, MaxBlocked = 500;
// Requests the player sent, hidden ones included: 20 a day for the 30 days a request lives, and the pending ones.
constexpr std::size_t MaxOutgoing = 700;
// The highest table number a room has.
constexpr int MaxTable = 7;

// What a player in a room is doing there (the wire's `status`).
enum class Status : std::uint8_t { Idle, Queued, Ready, Fighting, Watching, Training };
// What an online player outside a room is doing.
enum class Activity : std::uint8_t { Menus, OfflinePlay };

// A friend's main fighter. Points and tier are shown together or not at all.
struct Main {
	std::uint8_t fighter = 0;
	bool showsPoints = false;
	std::uint64_t points = 0;
	std::uint8_t tier = 0;
};

// A public room a friend is in.
struct Room {
	// 32 lower-case hex digits: the id the Public rooms list joins by.
	std::string id, name;
	std::uint8_t members = 0, capacity = 0;
	// Open, not locked, not full, and of this player's build.
	bool joinable = false;
};

// Where a friend is, as this player may see it. A friend who appears offline is Offline.
struct Presence {
	enum class Kind : std::uint8_t { Offline, Online, InRoom } kind = Kind::Offline;
	// Offline: when they were last seen (unix seconds), 0 when unknown.
	std::uint64_t seenAt = 0;
	// Online.
	Activity activity = Activity::Menus;
	// InRoom: what they do there, and at which table (-1: none). A public
	// room is `room` (hasRoom), a private one `privateRoom`; neither is a
	// public room that is no longer open and listed.
	Status status = Status::Idle;
	int table = -1;
	bool hasRoom = false, privateRoom = false;
	Room room;
};

// The wins between this player and a friend, from this player's side, and their last game.
struct Record {
	std::uint64_t won = 0, lost = 0, lastAt = 0;
};

struct Friend {
	// "fr_...": the name both sides use for the friendship.
	std::string id, name;
	CountryCode country;
	std::uint64_t since = 0;
	bool hasMain = false;
	Main main;
	// The class as last shown, or the developer mark; none when they do not take part or classes are hidden.
	SkillBadge badge;
	Presence presence;
	bool hasRecord = false;
	Record record;
};

// A request this player received.
struct Incoming {
	// "rq_..."
	std::string id, name;
	CountryCode country;
	std::uint64_t sentAt = 0;
	// Whether they have played, and how often: games 0 when never.
	std::uint64_t games = 0, lastGameAt = 0;
};

// A request this player sent, hidden ones included: those stay "sent".
struct Outgoing {
	std::string id, label;
	std::uint64_t sentAt = 0;
};

// The room an invite offers: a public room by its id, or a private room by its own invitation.
struct InviteRoom {
	enum class Kind : std::uint8_t { RoomId, Secret } kind = Kind::RoomId;
	std::string value;
};

// An invite this player received that is still live.
struct Invite {
	// "iv_..."; the inviting friend "fr_...".
	std::string id, friendId;
	InviteRoom room;
	std::uint64_t expiresAt = 0;
};

struct Blocked {
	// "bl_..."
	std::string id, name;
	std::uint64_t at = 0;
};

// The player's friends as the bridge last answered.
struct State {
	enum class Kind : std::uint8_t {
		// No answer yet, or the service has no friends.
		Unavailable,
		// No Discord account is connected to the Ember ID: friends belong to one.
		NoAccount,
		// The rest of the state is the bridge's answer.
		Listed,
	} kind = Kind::Unavailable;
	// The player's friend code, XXXX-XXXX.
	std::string code;
	// Friends see this player as offline.
	bool appearOffline = false;
	// In no order: the interface groups and sorts.
	std::vector<Friend> friends;
	std::vector<Incoming> incoming;
	std::vector<Outgoing> outgoing;
	std::vector<Invite> invites;
	std::vector<Blocked> blocked;

	const Friend* FindFriend(const std::string& id) const {
		for (const auto& entry : friends) if (entry.id == id) return &entry;
		return nullptr;
	}
	const Invite* FindInvite(const std::string& id) const {
		for (const auto& entry : invites) if (entry.id == id) return &entry;
		return nullptr;
	}
};

} } }
