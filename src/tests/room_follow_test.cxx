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
// A follower started in `room`.
RoomFollow Following(const Snapshot& room) {
	RoomFollow follow;
	follow.Start(room);
	return follow;
}
// The room takes actions and no watched game shows on this PC.
constexpr FollowGate Free{true, false};
// The table following moves to now, or -1; it never asks anything else of a
// follower that has not stopped watching.
template <class Follow> int Move(Follow&& follow, const Snapshot& room, std::uint64_t nowMs, FollowGate gate = Free) {
	const auto step = follow.Next(room, nowMs, gate);
	EXPECT(!step || step.kind == ActionKind::Watch);
	return step.table;
}

// The watched table's pair stood up: the follower moves to the game being
// played, and once the room shows it there, decides nothing more until that
// table stops too.
void IdleToPlayingMoves() {
	auto room = Room();
	Seat(room.tables[2], 1, 2); Play(room.tables[2]);
	Watch(room, 0);
	auto follow = Following(room);
	EXPECT(Move(follow, room, 0) == 2);
	follow.Watched(2, 0);
	EXPECT(Move(follow, room, 100) == -1);
	Watch(room, 2);
	EXPECT(Move(follow, room, 200) == -1);
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
	EXPECT(Move(follow, room, 0) == -1);
	End(room.tables[0]);
	EXPECT(Move(follow, room, 1000) == -1);
	EXPECT(Move(follow, room, 1000 + FollowRematchGraceMs - 1) == -1);
	room.tables[0].phase = TablePhase::Ready;
	EXPECT(Move(follow, room, 1000 + FollowRematchGraceMs + 500) == -1);
	Play(room.tables[0]);
	EXPECT(Move(follow, room, 9000) == -1);
	// The rematch ends and nothing starts in the grace: the room's other game takes it.
	End(room.tables[0]);
	EXPECT(Move(follow, room, 10000) == -1);
	EXPECT(Move(follow, room, 10000 + FollowRematchGraceMs) == 1);
}

// After the game it watched, a new pair at the table (a winner's next
// challenger) is no reason to stay while another table plays.
void NewPairDoesNotHold() {
	auto room = Room();
	Seat(room.tables[0], 1, 2);
	Seat(room.tables[3], 5, 6); Play(room.tables[3]);
	Watch(room, 0); Play(room.tables[0]);
	auto follow = Following(room);
	EXPECT(Move(follow, room, 0) == -1);
	End(room.tables[0]);
	room.tables[0].p2 = 7;
	EXPECT(Move(follow, room, 100) == 3);
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
	EXPECT(Move(follow, room, 0) == -1);
	End(room.tables[1]);
	room.tables[1].p2 = 5;
	EXPECT(Move(follow, room, 1000) == -1);
	EXPECT(Move(follow, room, 1000 + FollowNextGameMs - 1) == -1);
	room.tables[1].ready[0] = room.tables[1].ready[1] = true; room.tables[1].phase = TablePhase::Ready;
	EXPECT(Move(follow, room, 1000 + FollowNextGameMs) == -1);
	Play(room.tables[1]);
	EXPECT(Move(follow, room, 30000) == -1);
	// The same wait, run out: the other game takes it.
	auto late = Room();
	Seat(late.tables[1], 1, 2); Seat(late.tables[2], 3, 4); Play(late.tables[2]);
	Watch(late, 1);
	auto waiting = Following(late);
	EXPECT(Move(waiting, late, 0) == -1);
	EXPECT(Move(waiting, late, FollowNextGameMs) == 2);
	// A half-empty table keeps nobody waiting.
	late.tables[1].p2 = 0;
	auto alone = Following(late);
	EXPECT(Move(alone, late, 0) == 2);
}

// With several games on, every client chooses the lowest-numbered one; a
// table that takes no more spectators, or whose last game is still closing,
// is passed over, and a table beyond a Classic room's count is never chosen.
void LowestPlayableTableWins() {
	auto room = Room();
	for (const int index : {1, 2, 3}) { Seat(room.tables[index], static_cast<MemberId>(10 + index), static_cast<MemberId>(20 + index)); Play(room.tables[index]); }
	Watch(room, 0);
	EXPECT(Move(Following(room), room, 0) == 1);
	for (MemberId id = 100; room.tables[1].spectators.size() + room.tables[1].watchingNext.size() < MaxSpectators; ++id)
		room.tables[1].watchingNext.push_back(id);
	EXPECT(Move(Following(room), room, 0) == 2);
	room.terminalPending[2] = true;
	EXPECT(Move(Following(room), room, 0) == 3);
	room.tableCount = 3;
	EXPECT(Move(Following(room), room, 0) == -1);
	room.tableCount = 4;
	room.localTerminalPending = true;
	EXPECT(Move(Following(room), room, 0) == -1);
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
	EXPECT(Move(follow, room, 0) == 1);
	follow.Watched(1, 0);
	EXPECT(Move(follow, room, FollowMoveTimeoutMs - 1) == -1);
	EXPECT(follow.Followed(room) == 1);
	for (MemberId id = 100; room.tables[1].spectators.size() + room.tables[1].watchingNext.size() < MaxSpectators; ++id)
		room.tables[1].spectators.push_back(id);
	EXPECT(Move(follow, room, FollowMoveTimeoutMs) == 2);
	EXPECT(follow.On() && follow.Followed(room) == 0);
	// Nothing moves while the room cannot take it, or this PC still shows a watched game.
	EXPECT(Move(follow, room, FollowMoveTimeoutMs, {false, false}) == -1 &&
		Move(follow, room, FollowMoveTimeoutMs, {true, true}) == -1);
}

// Following ends once the member sits down, queues, stops watching or leaves
// the room, and in a room bound to a tournament, whose binding seats and
// places everyone itself.
void EndsWithAPlace() {
	auto room = Room();
	Seat(room.tables[1], 1, 2); Play(room.tables[1]);
	Watch(room, 0);
	auto seated = room;
	seated.members[0].table = 0; seated.members[0].seat = 0; seated.tables[0].p1 = Me;
	auto follow = Following(room);
	EXPECT(Move(follow, seated, 0) == -1 && !follow.On());
	auto queued = room;
	queued.tables[1].queue.push_back(Me);
	follow = Following(room);
	EXPECT(Move(follow, queued, 0) == -1 && !follow.On());
	auto stopped = room;
	stopped.tables[0].spectators.clear();
	follow = Following(room);
	EXPECT(Move(follow, stopped, 0) == -1 && !follow.On());
	auto other = room;
	other.roomEpoch = 6;
	follow = Following(room);
	EXPECT(Move(follow, other, 0) == -1 && !follow.On());
	auto gone = room;
	gone.members.clear();
	follow = Following(room);
	EXPECT(Move(follow, gone, 0) == -1 && !follow.On());
	auto bound = room;
	bound.tournament.matchId = "emt_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a12";
	follow = Following(room);
	EXPECT(Move(follow, room, 0) == 1);
	follow = Following(room);
	EXPECT(Move(follow, bound, 0) == -1 && !follow.On());
	// Stopping by hand is the same end.
	follow = Following(room);
	follow.Stop();
	EXPECT(Move(follow, room, 0) == -1 && !follow.On());
}

// A lock-in pressed before following started, on a snapshot older than the
// one it lands in, ends following there: following never holds a start, even
// at a table that is about to start and still waits on this watcher's last
// game. The lock-in stays, the member's own choice.
void LockInEndsFollowing() {
	auto room = Room();
	Seat(room.tables[0], 1, 2);
	Seat(room.tables[1], 3, 4); Play(room.tables[1]);
	Watch(room, 0);
	auto follow = Following(room);
	auto locked = room;
	locked.members[0].spectatorLocked = true;
	locked.tables[0].phase = TablePhase::Ready;
	locked.tables[0].ready[0] = locked.tables[0].ready[1] = true;
	locked.tables[0].spectatorHold = true;
	locked.localTerminalPending = true;
	EXPECT(Move(follow, locked, 0) == -1 && !follow.On());
	locked.tables[0].phase = TablePhase::Waiting;
	locked.localTerminalPending = false;
	EXPECT(Move(follow, locked, 60000) == -1 && !follow.On());
}

// Stop watching wins over a move still on its way. Watching idle table 0,
// following sent Watch 1; the member's Unwatch 0 goes out after it. The room
// takes the Watch first, so the Unwatch finds nothing at 0 and the member is
// left watching 1: following, already off, leaves 1 once, then asks nothing.
void StopWatchingBeatsMove() {
	auto room = Room();
	Seat(room.tables[1], 1, 2); Play(room.tables[1]);
	Seat(room.tables[2], 3, 4); Play(room.tables[2]);
	Watch(room, 0);
	auto follow = Following(room);
	EXPECT(Move(follow, room, 0) == 1);
	follow.Watched(1, 0);
	follow.StoppedWatching(room, 0, 0);
	EXPECT(!follow.On());
	// Neither answer is in yet.
	EXPECT(!follow.Next(room, 100, Free));
	Watch(room, 1);
	auto step = follow.Next(room, 200, {false, false});
	EXPECT(!step);
	step = follow.Next(room, 300, {true, true});
	EXPECT(step.kind == ActionKind::Unwatch && step.table == 1);
	// The room takes that Unwatch: nothing is watched.
	room.tables[1].watchingNext.clear();
	for (std::uint64_t now = 400; now < 60000; now += 1000) EXPECT(!follow.Next(room, now, Free));
	// The other order: the room refused the Watch and took the Unwatch, so
	// nothing is watched and nothing is left.
	auto refused = Room();
	Seat(refused.tables[1], 1, 2); Play(refused.tables[1]);
	Watch(refused, 0);
	auto again = Following(refused);
	EXPECT(Move(again, refused, 0) == 1);
	again.Watched(1, 0);
	again.StoppedWatching(refused, 0, 0);
	refused.tables[0].spectators.clear();
	EXPECT(!again.Next(refused, 100, Free));
	Watch(refused, 1);
	EXPECT(!again.Next(refused, 200, Free));
	// A table chosen after stopping is the member's last word.
	auto chosen = Room();
	Seat(chosen.tables[1], 1, 2); Play(chosen.tables[1]);
	Watch(chosen, 0);
	auto last = Following(chosen);
	EXPECT(Move(last, chosen, 0) == 1);
	last.Watched(1, 0);
	last.StoppedWatching(chosen, 0, 0);
	last.Watched(1, 50);
	Watch(chosen, 1);
	EXPECT(!last.Next(chosen, 100, Free) && !last.On());
}

// Stop watching holds until the room shows it. Pressed twice before any
// answer, with both Unwatches refused after the move landed, it still leaves
// the move's table; an Unwatch that did not go is tried again after the wait;
// it ends once nothing is watched, or with the room.
void StopWatchingHolds() {
	auto room = Room();
	Seat(room.tables[1], 1, 2); Play(room.tables[1]);
	Watch(room, 0);
	auto follow = Following(room);
	EXPECT(Move(follow, room, 0) == 1);
	follow.Watched(1, 0);
	follow.StoppedWatching(room, 0, 10);
	follow.StoppedWatching(room, 0, 20);
	EXPECT(!follow.On() && !follow.Next(room, 30, Free));
	Watch(room, 1);
	auto step = follow.Next(room, 40, Free);
	EXPECT(step.kind == ActionKind::Unwatch && step.table == 1);
	// It did not go: the shell notes it as tried.
	follow.StoppedWatching(room, 1, 40);
	EXPECT(!follow.Next(room, 40 + FollowMoveTimeoutMs - 1, Free));
	EXPECT(!follow.Next(room, 40 + FollowMoveTimeoutMs, {false, false}));
	step = follow.Next(room, 40 + FollowMoveTimeoutMs, {true, true});
	EXPECT(step.kind == ActionKind::Unwatch && step.table == 1);
	follow.StoppedWatching(room, 1, 40 + FollowMoveTimeoutMs);
	room.tables[1].watchingNext.clear();
	EXPECT(!follow.Next(room, 60000, Free));
	Watch(room, 1);
	EXPECT(!follow.Next(room, 120000, Free));
	// Another room, or the member gone from it, ends the intent with it.
	auto left = Room();
	Watch(left, 2);
	RoomFollow stopped;
	stopped.StoppedWatching(left, 2, 0);
	auto next = left;
	next.roomEpoch = 7;
	EXPECT(!stopped.Next(next, FollowMoveTimeoutMs, Free));
	EXPECT(!stopped.Next(left, 2 * FollowMoveTimeoutMs, Free));
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
	const int next = Move(follow, room, 0);
	EXPECT(next == 1);
	EXPECT(apply(viewer, next, ActionKind::Watch).accepted);
	follow.Watched(next, 0);
	room = authority.SnapshotFor(viewer);
	EXPECT(WatchedTable(room, viewer) == 1 && room.tables[0].spectators.empty());
	EXPECT(FindMember(room, viewer)->status == MemberStatus::WatchingNext && !FindMember(room, viewer)->spectatorLocked);
	EXPECT(Move(follow, room, 100) == -1 && follow.On());
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
	LockInEndsFollowing();
	StopWatchingBeatsMove();
	StopWatchingHolds();
	AuthorityTakesTheMove();
	if (!TEST_FAILURES()) std::printf("Room follow tests passed\n");
	return TEST_EXIT_CODE();
}
