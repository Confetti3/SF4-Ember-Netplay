// Follow the room (session/RoomFollow.hxx): a spectator that follows moves by
// itself, one ordinary Watch at a time, to the room's next game whenever the
// table it watches stops playing, and stops following once it sits or queues.
#include "../session/RoomFollow.hxx"
#include "../session/RoomModel.hxx"

#include <string>

#include "test_support.hxx"

using namespace sf4e::room;

namespace {
constexpr MemberId Me = 9;

// A Classic room of four tables with the local member in it, watching nothing.
Snapshot Room() {
	Snapshot room;
	room.roomEpoch = 5;
	room.localMember = Me;
	for (std::size_t i = 0; i < room.tables.size(); ++i) room.tables[i].id = static_cast<std::uint8_t>(i);
	Member me; me.id = Me; me.name = "Viewer";
	room.members.push_back(me);
	return room;
}
void Seat(Table& table, MemberId p1, MemberId p2) { table.p1 = p1; table.p2 = p2; table.phase = TablePhase::Waiting; }
void Play(Table& table) { table.phase = TablePhase::Playing; ++table.matchGeneration; }
// The game at `table` ends: the room folds those waiting for the next one into
// its spectators, and the pair is seated again between games.
void End(Table& table) {
	table.phase = TablePhase::Waiting;
	table.spectators.insert(table.spectators.end(), table.watchingNext.begin(), table.watchingNext.end());
	table.watchingNext.clear();
}
// What the room shows once it took a Watch of `index` from `member`: watching
// ends elsewhere, and during a game it waits for the next one.
void Watch(Snapshot& room, int index, MemberId member = Me) {
	for (auto& table : room.tables) {
		table.spectators.erase(std::remove(table.spectators.begin(), table.spectators.end(), member), table.spectators.end());
		table.watchingNext.erase(std::remove(table.watchingNext.begin(), table.watchingNext.end(), member), table.watchingNext.end());
	}
	auto& table = room.tables[index];
	(table.phase == TablePhase::Playing ? table.watchingNext : table.spectators).push_back(member);
}
// A follower started at `watched`, whose first decision has been made at 0 ms.
RoomFollow Following(const Snapshot& room) {
	RoomFollow follow;
	follow.Start(room);
	return follow;
}

// The watched table's pair stood up: the follower moves to the game being
// played, and once the room shows it there, decides nothing more until that
// table stops too.
void IdleToPlayingMoves() {
	auto room = Room();
	Seat(room.tables[2], 1, 2); Play(room.tables[2]);
	Watch(room, 0);
	auto follow = Following(room);
	EXPECT(follow.Next(room, 0, true) == 2);
	follow.Moved(2, 0);
	EXPECT(follow.Next(room, 100, true) == -1);
	Watch(room, 2);
	EXPECT(follow.Next(room, 200, true) == -1);
	EXPECT(follow.On() && follow.Followed(room) == 2);
	// It waits there for the next game, which nobody else is playing.
	EXPECT(room.tables[2].watchingNext.size() == 1);
}

// A pair that stays seated after the game the follower watched keeps it for
// FollowRematchGraceMs, so an Arcade winner's next set at the same table is
// watched; a rematch that starts in time keeps it there.
void RematchKeepsFollower() {
	auto room = Room();
	Seat(room.tables[0], 1, 2);
	Seat(room.tables[1], 3, 4); Play(room.tables[1]);
	Watch(room, 0); Play(room.tables[0]);
	auto follow = Following(room);
	EXPECT(follow.Next(room, 0, true) == -1);
	End(room.tables[0]);
	EXPECT(follow.Next(room, 1000, true) == -1);
	EXPECT(follow.Next(room, 1000 + FollowRematchGraceMs - 1, true) == -1);
	room.tables[0].phase = TablePhase::Ready;
	EXPECT(follow.Next(room, 1000 + FollowRematchGraceMs + 500, true) == -1);
	Play(room.tables[0]);
	EXPECT(follow.Next(room, 9000, true) == -1);
	// The rematch ends and nothing starts in the grace: the room's other game takes it.
	End(room.tables[0]);
	EXPECT(follow.Next(room, 10000, true) == -1);
	EXPECT(follow.Next(room, 10000 + FollowRematchGraceMs, true) == 1);
}

// After the game it watched, a new pair at the table (a winner's next
// challenger) is no reason to stay while another table plays.
void NewPairDoesNotHold() {
	auto room = Room();
	Seat(room.tables[0], 1, 2);
	Seat(room.tables[3], 5, 6); Play(room.tables[3]);
	Watch(room, 0); Play(room.tables[0]);
	auto follow = Following(room);
	EXPECT(follow.Next(room, 0, true) == -1);
	End(room.tables[0]);
	room.tables[0].p2 = 7;
	EXPECT(follow.Next(room, 100, true) == 3);
}

// A follower that arrives mid-game waits out that game in watchingNext. When
// it ends, the table keeps the follower for its next game while both seats
// are filled, here with a new challenger, rather than sending it on before it
// has been shown a game; and for no longer than FollowNextGameMs.
void ArrivalWaitsForItsGame() {
	auto room = Room();
	Seat(room.tables[1], 1, 2); Play(room.tables[1]);
	Seat(room.tables[2], 3, 4); Play(room.tables[2]);
	Watch(room, 1);
	auto follow = Following(room);
	EXPECT(follow.Next(room, 0, true) == -1);
	End(room.tables[1]);
	room.tables[1].p2 = 5;
	EXPECT(follow.Next(room, 1000, true) == -1);
	EXPECT(follow.Next(room, 1000 + FollowNextGameMs - 1, true) == -1);
	room.tables[1].ready[0] = room.tables[1].ready[1] = true; room.tables[1].phase = TablePhase::Ready;
	EXPECT(follow.Next(room, 1000 + FollowNextGameMs, true) == -1);
	Play(room.tables[1]);
	EXPECT(follow.Next(room, 30000, true) == -1);
	// The same wait, run out: the other game takes it.
	auto late = Room();
	Seat(late.tables[1], 1, 2); Seat(late.tables[2], 3, 4); Play(late.tables[2]);
	Watch(late, 1);
	auto waiting = Following(late);
	EXPECT(waiting.Next(late, 0, true) == -1);
	EXPECT(waiting.Next(late, FollowNextGameMs, true) == 2);
	// A half-empty table keeps nobody waiting.
	late.tables[1].p2 = 0;
	auto alone = Following(late);
	EXPECT(alone.Next(late, 0, true) == 2);
}

// With several games on, every client chooses the lowest-numbered one; a
// table that takes no more spectators, or whose last game is still closing,
// is passed over, and a table beyond a Classic room's count is never chosen.
void LowestPlayableTableWins() {
	auto room = Room();
	for (const int index : {1, 2, 3}) { Seat(room.tables[index], static_cast<MemberId>(10 + index), static_cast<MemberId>(20 + index)); Play(room.tables[index]); }
	Watch(room, 0);
	EXPECT(Following(room).Next(room, 0, true) == 1);
	for (MemberId id = 100; room.tables[1].spectators.size() + room.tables[1].watchingNext.size() < MaxSpectators; ++id)
		room.tables[1].watchingNext.push_back(id);
	EXPECT(Following(room).Next(room, 0, true) == 2);
	room.terminalPending[2] = true;
	EXPECT(Following(room).Next(room, 0, true) == 3);
	room.tableCount = 3;
	EXPECT(Following(room).Next(room, 0, true) == -1);
	room.tableCount = 4;
	room.localTerminalPending = true;
	EXPECT(Following(room).Next(room, 0, true) == -1);
}

// One move at a time: none is decided while one is on its way. A move the
// room refuses leaves the follower where it was; after FollowMoveTimeoutMs it
// decides again from what the room shows, which here passes over the table
// that filled up.
void OneMoveInFlight() {
	auto room = Room();
	Seat(room.tables[1], 1, 2); Play(room.tables[1]);
	Seat(room.tables[2], 3, 4); Play(room.tables[2]);
	Watch(room, 0);
	auto follow = Following(room);
	EXPECT(follow.Next(room, 0, true) == 1);
	follow.Moved(1, 0);
	EXPECT(follow.Next(room, FollowMoveTimeoutMs - 1, true) == -1);
	EXPECT(follow.Followed(room) == 1);
	for (MemberId id = 100; room.tables[1].spectators.size() + room.tables[1].watchingNext.size() < MaxSpectators; ++id)
		room.tables[1].spectators.push_back(id);
	EXPECT(follow.Next(room, FollowMoveTimeoutMs, true) == 2);
	EXPECT(follow.On() && follow.Followed(room) == 0);
	// Nothing moves while the room cannot take it, or this PC still shows a watched game.
	EXPECT(follow.Next(room, FollowMoveTimeoutMs, false) == -1);
}

// Following ends once the member sits down, queues, stops watching or leaves
// the room, and in a room bound to a tournament.
void EndsWithAPlace() {
	auto room = Room();
	Seat(room.tables[1], 1, 2); Play(room.tables[1]);
	Watch(room, 0);
	auto seated = room;
	seated.members[0].table = 0; seated.members[0].seat = 0; seated.tables[0].p1 = Me;
	auto follow = Following(room);
	EXPECT(follow.Next(seated, 0, true) == -1 && !follow.On());
	auto queued = room;
	queued.tables[1].queue.push_back(Me);
	follow = Following(room);
	EXPECT(follow.Next(queued, 0, true) == -1 && !follow.On());
	auto stopped = room;
	stopped.tables[0].spectators.clear();
	follow = Following(room);
	EXPECT(follow.Next(stopped, 0, true) == -1 && !follow.On());
	auto other = room;
	other.roomEpoch = 6;
	follow = Following(room);
	EXPECT(follow.Next(other, 0, true) == -1 && !follow.On());
	auto gone = room;
	gone.members.clear();
	follow = Following(room);
	EXPECT(follow.Next(gone, 0, true) == -1 && !follow.On());
	// Stopping by hand is the same end.
	follow = Following(room);
	follow.Stop();
	EXPECT(follow.Next(room, 0, true) == -1 && !follow.On());
}

// Against the room's own authority: the move is one Watch, which takes the
// follower from the idle table to the game being played (waiting for its next
// game) with no Unwatch first, and never locks it in.
void AuthorityTakesTheMove() {
	RoomAuthority authority{"Follow", 8, 41};
	authority.AdvanceTime(1000);
	const auto join = [&](const char* name, int index, bool host) {
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(index)}, host);
		EXPECT(result.accepted);
		return result.accepted ? result.snapshot.members.back().id : MemberId(0);
	};
	const auto apply = [&](MemberId member, int table, ActionKind kind) {
		Action action;
		action.kind = kind;
		action.roomEpoch = authority.SnapshotView().roomEpoch;
		action.revision = authority.SnapshotView().revision;
		action.table = static_cast<std::uint8_t>(table);
		action.tableRevision = authority.SnapshotView().tables[table].revision;
		action.actionId = member * 1000 + authority.SnapshotView().revision + 1;
		return authority.Apply(member, action);
	};
	join("Host", 0, true);
	const auto p1 = join("P1", 1, false), p2 = join("P2", 2, false), viewer = join("Viewer", 3, false);
	EXPECT(apply(p1, 1, ActionKind::Queue).accepted && apply(p2, 1, ActionKind::Queue).accepted);
	EXPECT(apply(p1, 1, ActionKind::Ready).accepted && apply(p2, 1, ActionKind::Ready).accepted);
	EXPECT(authority.BeginMatch(1, p1, p2).accepted);
	EXPECT(apply(viewer, 0, ActionKind::Watch).accepted);
	auto room = authority.SnapshotFor(viewer);
	RoomFollow follow;
	follow.Start(room);
	const int next = follow.Next(room, 0, true);
	EXPECT(next == 1);
	EXPECT(apply(viewer, next, ActionKind::Watch).accepted);
	follow.Moved(next, 0);
	room = authority.SnapshotFor(viewer);
	EXPECT(WatchedTable(room, viewer) == 1 && room.tables[0].spectators.empty());
	EXPECT(FindMember(room, viewer)->status == MemberStatus::WatchingNext && !FindMember(room, viewer)->spectatorLocked);
	EXPECT(follow.Next(room, 100, true) == -1 && follow.On());
}
}

int main() {
	IdleToPlayingMoves();
	RematchKeepsFollower();
	NewPairDoesNotHold();
	ArrivalWaitsForItsGame();
	LowestPlayableTableWins();
	OneMoveInFlight();
	EndsWithAPlace();
	AuthorityTakesTheMove();
	if (!TEST_FAILURES()) std::printf("Room follow tests passed\n");
	return TEST_EXIT_CODE();
}
