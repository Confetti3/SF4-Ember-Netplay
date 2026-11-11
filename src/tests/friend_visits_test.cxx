// Going to a friend (netplay/FriendVisits.hxx): where a Join, a Watch and a
// taken invite lead for every state a friend can be in, and a visit to a
// public room under every order of the player being free, asked, answering,
// leaving, getting a ticket and arriving.
#include "../netplay/FriendVisits.hxx"

#include <cstdio>
#include <string>

#include "test_support.hxx"

using namespace sf4e::netplay::friends;
using Refusal = Destination::Refusal;
using Step = Visit::Step;

namespace {
const std::string FriendId = "fr_11111111-1111-4111-8111-111111111111";
const std::string OtherId = "fr_22222222-2222-4222-8222-222222222222";
const std::string InviteId = "iv_11111111-1111-4111-8111-111111111111";
const std::string RoomId = "0f0123456789abcdef0123456789abcd";
const std::string Secret = "emd1:" + std::string(122, 'A');

Friend InRoom(const std::string& id, Status status, int table) {
	Friend entry;
	entry.id = id;
	entry.name = "Player 3";
	entry.presence.kind = Presence::Kind::InRoom;
	entry.presence.status = status;
	entry.presence.table = table;
	entry.presence.hasRoom = true;
	entry.presence.room.id = RoomId;
	entry.presence.room.name = "Player 1's room";
	entry.presence.room.members = 3;
	entry.presence.room.capacity = 16;
	entry.presence.room.joinable = true;
	return entry;
}

State Listed() {
	State state;
	state.kind = State::Kind::Listed;
	return state;
}

void JoiningAFriend() {
	auto state = Listed();
	state.friends.push_back(InRoom(FriendId, Status::Idle, -1));
	auto where = ToFriend(state, FriendId, false);
	EXPECT(where.kind == Destination::Kind::PublicRoom && where.roomId == RoomId && where.watchTable == -1 && where.name == "Player 3" && where.Any());

	// Not joinable: full, locked, closed or of another build.
	state.friends[0].presence.room.joinable = false;
	where = ToFriend(state, FriendId, false);
	EXPECT(!where.Any() && where.refusal == Refusal::NotJoinable && where.name == "Player 3");
	state.friends[0].presence.room.joinable = true;

	// A private room is joined by invite only.
	state.friends[0].presence.hasRoom = false;
	state.friends[0].presence.privateRoom = true;
	EXPECT(ToFriend(state, FriendId, false).refusal == Refusal::PrivateRoom && !ToFriend(state, FriendId, false).Any());
	// A public room that is no longer open and listed names nothing to go to.
	state.friends[0].presence.privateRoom = false;
	EXPECT(ToFriend(state, FriendId, false).refusal == Refusal::NotInRoom);

	// Online and offline friends are in no room.
	state.friends[0].presence = Presence();
	EXPECT(ToFriend(state, FriendId, false).refusal == Refusal::NotInRoom);
	state.friends[0].presence.kind = Presence::Kind::Online;
	EXPECT(ToFriend(state, FriendId, false).refusal == Refusal::NotInRoom);

	// A friend the state no longer holds, as when a press was made on an older list.
	EXPECT(ToFriend(state, OtherId, false).refusal == Refusal::Gone && ToFriend(state, OtherId, false).name.empty());
	EXPECT(ToFriend(Listed(), FriendId, true).refusal == Refusal::Gone);
}

void WatchingAFriend() {
	auto state = Listed();
	state.friends.push_back(InRoom(FriendId, Status::Fighting, 2));
	auto where = ToFriend(state, FriendId, true);
	EXPECT(where.kind == Destination::Kind::PublicRoom && where.roomId == RoomId && where.watchTable == 2);
	// The same friend, joined rather than watched, names no table.
	EXPECT(ToFriend(state, FriendId, false).watchTable == -1);
	// Only a friend who fights can be watched.
	for (const auto status : {Status::Idle, Status::Queued, Status::Ready, Status::Watching, Status::Training}) {
		state.friends[0].presence.status = status;
		where = ToFriend(state, FriendId, true);
		EXPECT(!where.Any() && where.refusal == Refusal::NotFighting);
		EXPECT(ToFriend(state, FriendId, false).Any());
	}
	// And only one the bridge named a table of.
	state.friends[0].presence.status = Status::Fighting;
	state.friends[0].presence.table = -1;
	EXPECT(ToFriend(state, FriendId, true).refusal == Refusal::NotFighting);
	// Not joinable beats not fighting: the room is the first thing in the way.
	state.friends[0].presence.room.joinable = false;
	EXPECT(ToFriend(state, FriendId, true).refusal == Refusal::NotJoinable);
}

void TakingAnInvite() {
	auto state = Listed();
	state.friends.push_back(InRoom(FriendId, Status::Idle, -1));
	Invite publicInvite;
	publicInvite.id = InviteId;
	publicInvite.friendId = FriendId;
	publicInvite.room.kind = InviteRoom::Kind::RoomId;
	publicInvite.room.value = RoomId;
	publicInvite.expiresAt = 1000;
	state.invites.push_back(publicInvite);
	auto where = ToInvite(state, InviteId, 999);
	EXPECT(where.kind == Destination::Kind::PublicRoom && where.roomId == RoomId && where.watchTable == -1 && where.name == "Player 3");
	// The invite is for the room it names, whatever the friend does now and whether the friend is still listed.
	state.friends.clear();
	where = ToInvite(state, InviteId, 999);
	EXPECT(where.kind == Destination::Kind::PublicRoom && where.roomId == RoomId && where.name.empty());

	state.invites[0].room.kind = InviteRoom::Kind::Secret;
	state.invites[0].room.value = Secret;
	where = ToInvite(state, InviteId, 999);
	EXPECT(where.kind == Destination::Kind::PrivateInvite && where.secret == Secret && where.roomId.empty());

	// Over, or gone.
	EXPECT(ToInvite(state, InviteId, 1000).refusal == Refusal::Gone && ToInvite(state, InviteId, 5000).refusal == Refusal::Gone);
	EXPECT(ToInvite(state, "iv_2", 0).refusal == Refusal::Gone && !ToInvite(Listed(), InviteId, 0).Any());
}

Destination PublicRoom(int watchTable = -1) {
	Destination to;
	to.kind = Destination::Kind::PublicRoom;
	to.roomId = RoomId;
	to.watchTable = watchTable;
	to.name = "Player 3";
	return to;
}

// A PC at the main menu with nothing open, at `ms`, in room generation 5.
Surroundings Free(std::uint64_t ms) {
	Surroundings here;
	here.nowMs = ms;
	here.generation = 5;
	here.safeMenu = here.idle = here.canJoin = true;
	return here;
}

void AFreePlayerJoins() {
	Visit visit;
	EXPECT(!visit.Active() && visit.Tick(Free(0)) == Step::None);
	visit.Start("brg_1", PublicRoom(), 5, false, 1000);
	EXPECT(visit.Active() && !visit.Confirming() && visit.Revision() == 1 && visit.Bridge() == "brg_1" && visit.RoomId() == RoomId &&
		visit.Name() == "Player 3" && visit.WatchTable() == -1);
	// Nothing goes before the menu, the room control and the idle session allow.
	auto here = Free(1100);
	here.safeMenu = false;
	EXPECT(visit.Tick(here) == Step::None);
	here = Free(1100);
	here.canJoin = false;
	EXPECT(visit.Tick(here) == Step::None);
	EXPECT(visit.Tick(Free(1200)) == Step::Ticket);
	// The ticket is asked for once; its answer is awaited under its own id only.
	EXPECT(visit.Tick(Free(1300)) == Step::None && !visit.Awaiting(7));
	visit.Ticketed(7, 1300);
	EXPECT(visit.Awaiting(7) && !visit.Awaiting(8) && !visit.Awaiting(0) && visit.Tick(Free(1400)) == Step::None);
	visit.Admitted(true, 1500);
	EXPECT(!visit.Awaiting(7) && visit.Active());
	// Joining until the room shows this PC in it, then done: a Join has nothing after it.
	EXPECT(visit.Tick(Free(1600)) == Step::None && visit.Active());
	auto arrived = Free(1700);
	arrived.roomId = RoomId;
	EXPECT(visit.Tick(arrived) == Step::None && !visit.Active());
}

void WatchingOnceIn() {
	Visit visit;
	visit.Start("brg_1", PublicRoom(2), 5, false, 0);
	EXPECT(visit.WatchTable() == 2 && visit.Tick(Free(10)) == Step::Ticket);
	visit.Ticketed(3, 10);
	visit.Admitted(true, 20);
	auto arrived = Free(30);
	arrived.roomId = RoomId;
	arrived.idle = false;
	// In the room, but the room does not take an action yet.
	EXPECT(visit.Tick(arrived) == Step::None && visit.Active());
	EXPECT(visit.Tick(arrived) == Step::None);
	arrived.nowMs = 40;
	arrived.canAct = true;
	EXPECT(visit.Tick(arrived) == Step::Watch && !visit.Active());
	EXPECT(visit.WatchTable() == 2 && visit.Tick(arrived) == Step::None);
	// Another room is not the one.
	Visit other;
	other.Start("brg_1", PublicRoom(2), 5, false, 0);
	other.Tick(Free(10));
	other.Ticketed(1, 10);
	other.Admitted(true, 20);
	auto elsewhere = Free(30);
	elsewhere.roomId = "ff" + RoomId.substr(2);
	elsewhere.canAct = true;
	EXPECT(other.Tick(elsewhere) == Step::None && other.Active());
	// Already in the room when it starts: no ticket, straight to the table.
	Visit there;
	there.Start("brg_1", PublicRoom(1), 5, false, 0);
	auto inIt = Free(10);
	inIt.roomId = RoomId;
	inIt.idle = false;
	EXPECT(there.Tick(inIt) == Step::None && there.Active());
	inIt.canAct = true;
	EXPECT(there.Tick(inIt) == Step::Watch);
	Visit joining;
	joining.Start("brg_1", PublicRoom(), 5, false, 0);
	EXPECT(joining.Tick(inIt) == Step::None && !joining.Active());
}

void AskingFirst() {
	Visit visit;
	visit.Start("brg_1", PublicRoom(), 5, true, 1000);
	EXPECT(visit.Confirming() && visit.Active());
	// Nothing leaves while the question stands, whatever the player is in.
	auto in_game = Free(1100);
	in_game.idle = false;
	EXPECT(visit.Tick(in_game) == Step::None && visit.Confirming());
	// An answer for another question, or another room, is nobody's.
	EXPECT(!visit.Confirm(5, visit.Revision() + 1, 1200) && !visit.Confirm(6, visit.Revision(), 1200) && visit.Confirming());
	EXPECT(visit.Confirm(5, visit.Revision(), 1200) && !visit.Confirming() && visit.Active());
	EXPECT(!visit.Confirm(5, visit.Revision(), 1210));
	// Switching waits for a menu it can leave from, leaves once, and asks for the ticket when idle.
	auto menu = in_game;
	menu.nowMs = 1300;
	menu.safeMenu = false;
	EXPECT(visit.Tick(menu) == Step::None);
	menu.safeMenu = true;
	EXPECT(visit.Tick(menu) == Step::Leave);
	EXPECT(visit.Tick(menu) == Step::Leave);
	visit.LeaveQueued();
	EXPECT(visit.Tick(menu) == Step::None);
	menu.idle = true;
	menu.canJoin = false;
	EXPECT(visit.Tick(menu) == Step::None);
	menu.canJoin = true;
	EXPECT(visit.Tick(menu) == Step::Ticket);
}

// A question nobody answers lapses silently, and one about a room the player has left moves on.
void AQuestionLapses() {
	Visit visit;
	visit.Start("brg_1", PublicRoom(), 5, true, 1000);
	auto here = Free(1000 + Visit::AskMs - 1);
	here.idle = false;
	EXPECT(visit.Tick(here) == Step::None && visit.Active());
	here.nowMs = 1000 + Visit::AskMs;
	EXPECT(visit.Tick(here) == Step::None && !visit.Active());
	visit.Start("brg_1", PublicRoom(), 5, true, 1000);
	here = Free(1100);
	here.generation = 6;
	here.idle = false;
	EXPECT(visit.Tick(here) == Step::None && !visit.Active());
	// Cancel ends it, and an answer to it then is nobody's.
	visit.Start("brg_1", PublicRoom(), 5, true, 1000);
	const auto revision = visit.Revision();
	visit.Cancel();
	EXPECT(!visit.Active() && !visit.Confirm(5, revision, 1100));
}

// Each step that does not come ends the visit: a ticket never asked or never answered, a join that never shows.
void WhatDoesNotComeEnds() {
	Visit visit;
	visit.Start("brg_1", PublicRoom(), 5, false, 0);
	EXPECT(visit.Tick(Free(10)) == Step::Ticket);
	visit.Ticketed(0, 10);
	EXPECT(!visit.Active());
	visit.Start("brg_1", PublicRoom(), 5, false, 0);
	visit.Tick(Free(10));
	visit.Ticketed(4, 10);
	visit.Admitted(false, 20);
	EXPECT(!visit.Active() && !visit.Awaiting(4));
	// A ticket that never comes ends at the deadline, with a word to say.
	visit.Start("brg_1", PublicRoom(), 5, false, 0);
	visit.Tick(Free(10));
	visit.Ticketed(5, 10);
	EXPECT(visit.Tick(Free(10 + Visit::VisitMs - 1)) == Step::None);
	EXPECT(visit.Tick(Free(10 + Visit::VisitMs)) == Step::Ended && !visit.Active());
	// A join the room never shows.
	visit.Start("brg_1", PublicRoom(), 5, false, 0);
	visit.Tick(Free(10));
	visit.Ticketed(6, 10);
	visit.Admitted(true, 100);
	EXPECT(visit.Tick(Free(100 + Visit::JoinMs - 1)) == Step::None);
	EXPECT(visit.Tick(Free(100 + Visit::JoinMs)) == Step::Ended);
	// A player who never frees up the menu.
	visit.Start("brg_1", PublicRoom(), 5, false, 0);
	auto busy = Free(Visit::VisitMs - 1);
	busy.safeMenu = false;
	EXPECT(visit.Tick(busy) == Step::None);
	busy.nowMs = Visit::VisitMs;
	EXPECT(visit.Tick(busy) == Step::Ended);
	// A room entered by hand meanwhile is the player's own, and ends the visit quietly.
	visit.Start("brg_1", PublicRoom(), 5, false, 0);
	auto changed = Free(10);
	changed.generation = 6;
	EXPECT(visit.Tick(changed) == Step::None && !visit.Active());
}

// The newest visit replaces the one before, and the revision tells which question an answer is for.
void ANewVisitReplaces() {
	Visit visit;
	visit.Start("brg_1", PublicRoom(), 5, true, 0);
	const auto first = visit.Revision();
	visit.Start("brg_2", PublicRoom(1), 5, true, 10);
	EXPECT(visit.Revision() == first + 1 && visit.Bridge() == "brg_2" && visit.WatchTable() == 1 && visit.Confirming());
	EXPECT(!visit.Confirm(5, first, 20) && visit.Confirm(5, first + 1, 20));
	// Ticket answers for a visit that was replaced are not its.
	visit.Tick(Free(30));
	visit.Ticketed(9, 30);
	visit.Start("brg_1", PublicRoom(), 5, false, 40);
	EXPECT(!visit.Awaiting(9));
}
}

int main() {
	JoiningAFriend();
	WatchingAFriend();
	TakingAnInvite();
	AFreePlayerJoins();
	WatchingOnceIn();
	AskingFirst();
	AQuestionLapses();
	WhatDoesNotComeEnds();
	ANewVisitReplaces();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	return TEST_EXIT_CODE();
}
