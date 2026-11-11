#pragma once
// Going to a friend: where a Join, a Watch or a taken invite leads, and the
// state of a visit to a public room while it waits for the player to be free
// (docs/design/FRIENDS.md, "Client"). The runtime sends the requests and
// submits the commands; this decides, so the routing and the waiting are
// tested without a game (friend_visits_test.cxx).
//
// - Join goes to the friend's public room, by its id, when the bridge says it
//   is joinable. A friend in a private room is joined only by an invite, whose
//   secret goes through the Discord join's own path (discord::PendingInvite).
// - Watch is Join, then, once in, the room's own Watch on the friend's table.
//   Only a friend who is fighting can be watched.
// - A visit asks first when this PC is in a room or a game (`busy`), as a
//   Discord join does: nothing leaves until the player answers Switch, and
//   a Cancel or the room changing ends it. It then leaves, asks the bridge for
//   the room's ticket, joins with it and (for Watch) watches; each step waits
//   for the one before, and any of them that does not come in time ends the
//   visit.
#include "Friends.hxx"

#include <cstdint>
#include <string>

namespace sf4e { namespace netplay { namespace friends {

// Where a command to go to a friend leads.
struct Destination {
	enum class Kind : std::uint8_t { None, PublicRoom, PrivateInvite } kind = Kind::None;
	// Why there is none.
	enum class Refusal : std::uint8_t {
		None,
		// The friend, or the invite, is not in the state (it changed since the press).
		Gone,
		// Not in a room, or in one that is no longer open and listed.
		NotInRoom,
		// A private room is joined by invite only.
		PrivateRoom,
		// A public room that is full, locked, closed, or of another build.
		NotJoinable,
		// Watch: the friend is not fighting, or the bridge named no table.
		NotFighting,
	} refusal = Refusal::None;
	// PublicRoom: the room's id, and for a Watch the friend's table (-1: only join).
	std::string roomId;
	int watchTable = -1;
	// PrivateInvite: the room's own invitation.
	std::string secret;
	// The friend's name, for the question the player is asked.
	std::string name;

	bool Any() const { return kind != Kind::None; }
};

// A friend's public room, joined or, with `watch`, watched at their table.
inline Destination ToFriend(const State& state, const std::string& friendId, bool watch) {
	Destination destination;
	const Friend* entry = state.FindFriend(friendId);
	if (!entry) { destination.refusal = Destination::Refusal::Gone; return destination; }
	destination.name = entry->name;
	const Presence& presence = entry->presence;
	if (presence.kind != Presence::Kind::InRoom) destination.refusal = Destination::Refusal::NotInRoom;
	else if (presence.privateRoom) destination.refusal = Destination::Refusal::PrivateRoom;
	else if (!presence.hasRoom) destination.refusal = Destination::Refusal::NotInRoom;
	else if (!presence.room.joinable) destination.refusal = Destination::Refusal::NotJoinable;
	else if (watch && (presence.status != Status::Fighting || presence.table < 0)) destination.refusal = Destination::Refusal::NotFighting;
	if (destination.refusal != Destination::Refusal::None) return destination;
	destination.kind = Destination::Kind::PublicRoom;
	destination.roomId = presence.room.id;
	if (watch) destination.watchTable = presence.table;
	return destination;
}

// Where an invite received leads: a public room by its id (joined whether
// or not the bridge still lists the inviter there), a private room by its secret.
inline Destination ToInvite(const State& state, const std::string& inviteId, std::uint64_t nowSeconds) {
	Destination destination;
	const Invite* invite = state.FindInvite(inviteId);
	if (!invite || invite->expiresAt <= nowSeconds) { destination.refusal = Destination::Refusal::Gone; return destination; }
	if (const Friend* inviter = state.FindFriend(invite->friendId)) destination.name = inviter->name;
	if (invite->room.kind == InviteRoom::Kind::RoomId) {
		destination.kind = Destination::Kind::PublicRoom;
		destination.roomId = invite->room.value;
	} else {
		destination.kind = Destination::Kind::PrivateInvite;
		destination.secret = invite->room.value;
	}
	return destination;
}

// What a visit sees of the world at one tick.
struct Surroundings {
	std::uint64_t nowMs = 0;
	// The room generation the player pressed under (netplay::Generation::room).
	std::uint64_t generation = 0;
	// The menu is one a room can be left or joined from; no room, game or
	// match is open (Idle); a room can be opened now.
	bool safeMenu = false, idle = false, canJoin = false;
	// The public room this PC is in (its id), or empty; and whether that
	// room takes an action now (joined, healthy, writable).
	std::string roomId;
	bool canAct = false;
};

// A visit to a friend's public room.
class Visit {
public:
	// What the runtime does next.
	enum class Step : std::uint8_t {
		None,
		// Leave the room this PC is in (the player said Switch).
		Leave,
		// Ask the bridge for the room's ticket.
		Ticket,
		// Watch the table, in the room just joined.
		Watch,
		// The visit ended: it took too long, or the player was moved elsewhere.
		Ended,
	};
	static constexpr std::uint64_t AskMs = 60000, VisitMs = 120000, JoinMs = 30000;

	bool Active() const { return phase_ != Phase::Idle; }
	// The player is asked whether to leave what they are in.
	bool Confirming() const { return phase_ == Phase::Asking; }
	// Changes with every visit started, so an answer is for the question that was asked.
	std::uint64_t Revision() const { return revision_; }
	const std::string& Bridge() const { return bridge_; }
	const std::string& RoomId() const { return roomId_; }
	const std::string& Name() const { return name_; }
	int WatchTable() const { return watchTable_; }

	// A new visit replaces the one before. `busy`: the player is in a room or
	// a game, so nothing happens until they say Switch.
	void Start(const std::string& bridge, const Destination& to, std::uint64_t generation, bool busy, std::uint64_t nowMs) {
		++revision_;
		bridge_ = bridge;
		roomId_ = to.roomId;
		name_ = to.name;
		watchTable_ = to.watchTable;
		generation_ = generation;
		switching_ = false;
		phase_ = busy ? Phase::Asking : Phase::Waiting;
		deadlineMs_ = nowMs + (busy ? AskMs : VisitMs);
	}
	void Cancel() { phase_ = Phase::Idle; }

	// The player's answer to the question, for the revision they saw and the room they pressed in.
	bool Confirm(std::uint64_t generation, std::uint64_t revision, std::uint64_t nowMs) {
		if (phase_ != Phase::Asking || revision != revision_ || generation != generation_) return false;
		phase_ = Phase::Waiting;
		switching_ = true;
		deadlineMs_ = nowMs + VisitMs;
		return true;
	}

	// The ticket was asked for as request `id`; 0 when it could not be, which ends the visit.
	void Ticketed(std::uint64_t id, std::uint64_t nowMs) {
		if (phase_ != Phase::Ticketing) return;
		if (!id) { Cancel(); return; }
		ticket_ = id;
		deadlineMs_ = nowMs + VisitMs;
	}
	bool Awaiting(std::uint64_t id) const { return phase_ == Phase::Ticketing && id && id == ticket_; }
	// The ticket came (`joined`: the join was submitted with it) or was refused.
	void Admitted(bool joined, std::uint64_t nowMs) {
		ticket_ = 0;
		if (!joined) { Cancel(); return; }
		phase_ = Phase::Joining;
		deadlineMs_ = nowMs + JoinMs;
	}

	// The Leave went out; it is not sent again.
	void LeaveQueued() { switching_ = false; }

	Step Tick(const Surroundings& here) {
		if (phase_ == Phase::Idle) return Step::None;
		// The question is dropped, unanswered, when the room it was about changes or nobody answers.
		if (phase_ == Phase::Asking) {
			if (here.nowMs >= deadlineMs_ || here.generation != generation_) Cancel();
			return Step::None;
		}
		if (here.nowMs >= deadlineMs_) { Cancel(); return Step::Ended; }
		const bool inRoom = !roomId_.empty() && here.roomId == roomId_;
		switch (phase_) {
		case Phase::Waiting:
			// Already there, as when a friend's invite is for the room this PC is in.
			if (inRoom) return Arrived();
			if (here.generation != generation_) { Cancel(); return Step::None; }
			if (!here.safeMenu) return Step::None;
			if (!here.idle) return switching_ ? Step::Leave : Step::None;
			if (!here.canJoin) return Step::None;
			phase_ = Phase::Ticketing;
			return Step::Ticket;
		case Phase::Joining:
			return inRoom ? Arrived() : Step::None;
		case Phase::Watching:
			// Once in the room and it takes an action.
			if (!inRoom || !here.canAct) return Step::None;
			Cancel();
			return Step::Watch;
		default:
			return Step::None;
		}
	}

private:
	enum class Phase : std::uint8_t { Idle, Asking, Waiting, Ticketing, Joining, Watching };

	// In the room: a Join is done, a Watch goes on to the table.
	Step Arrived() {
		if (watchTable_ < 0) { Cancel(); return Step::None; }
		phase_ = Phase::Watching;
		return Step::None;
	}

	Phase phase_ = Phase::Idle;
	std::string bridge_, roomId_, name_;
	int watchTable_ = -1;
	bool switching_ = false;
	std::uint64_t generation_ = 0, revision_ = 0, deadlineMs_ = 0, ticket_ = 0;
};

} } }
