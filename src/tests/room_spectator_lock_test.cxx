// A locked-in spectator holds the next start at its table for a bounded time
// while it retires the previous game.
#include "../session/RoomModel.hxx"

#include <algorithm>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

using namespace sf4e::room;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

static Action TableAction(const RoomAuthority& authority, MemberId member, ActionKind kind) {
	Action action;
	action.kind = kind;
	action.roomEpoch = authority.SnapshotView().roomEpoch;
	action.revision = authority.SnapshotView().revision;
	action.tableRevision = authority.SnapshotView().tables[0].revision;
	action.actionId = member * 1000 + authority.SnapshotView().revision + 1;
	return action;
}

static bool StartsMatch(const Result& result) {
	return std::any_of(result.events.begin(), result.events.end(),
		[](const Event& event) { return event.kind == Event::Kind::MatchReady; });
}

static bool StartsMatch(const std::vector<Event>& events) {
	Result result; result.events = events; return StartsMatch(result);
}

// Two fighters and a watching spectator at table 0, after one finished game
// both fighters have acknowledged. The spectator is still retiring it.
struct Room {
	RoomAuthority authority{"Spectator lock", 8, 41};
	MemberId p1 = 0, p2 = 0, spectator = 0;
	std::uint64_t finished = 0;

	explicit Room(bool locked) {
		authority.AdvanceTime(1000);
		Join("Host", 0, true);
		p1 = Join("P1", 1); p2 = Join("P2", 2); spectator = Join("Watcher", 3);
		for (const auto member : {p1, p2}) CHECK(Apply(member, ActionKind::Queue).accepted);
		CHECK(Apply(spectator, ActionKind::Watch).accepted);
		if (locked) CHECK(Lock(spectator, true).accepted);
		CHECK(ReadyBoth());
		CHECK(authority.BeginMatch(0, p1, p2).accepted);
		finished = authority.SnapshotView().tables[0].matchGeneration;
		CHECK(authority.EndMatch(0, finished, MatchResult::P1Win).accepted);
		CHECK(Acknowledge(p1) && Acknowledge(p2));
		CHECK(authority.SnapshotFor(spectator).localTerminalPending);
	}
	MemberId Join(const char* name, int index, bool host = false) {
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(index)}, host);
		CHECK(result.accepted);
		return result.accepted ? result.snapshot.members.back().id : 0;
	}
	Result Apply(MemberId member, ActionKind kind) { return authority.Apply(member, TableAction(authority, member, kind)); }
	Result Lock(MemberId member, bool locked) {
		auto action = TableAction(authority, member, ActionKind::LockSpectating);
		action.locked = locked;
		return authority.Apply(member, action);
	}
	bool Acknowledge(MemberId member) {
		auto action = TableAction(authority, member, ActionKind::AcknowledgeTerminal);
		action.matchGeneration = finished;
		return authority.Apply(member, action).accepted;
	}
	// True when the second Ready reported the table ready to start.
	bool ReadyBoth() {
		CHECK(Apply(p1, ActionKind::Ready).accepted);
		const auto second = Apply(p2, ActionKind::Ready);
		CHECK(second.accepted);
		return StartsMatch(second);
	}
	const Table& table() const { return authority.SnapshotView().tables[0]; }
	const Member& member(MemberId id) const {
		const auto& members = authority.SnapshotView().members;
		return *std::find_if(members.begin(), members.end(), [id](const Member& value) { return value.id == id; });
	}
};

static void TestAcknowledgementReleasesTheHold() {
	Room room(true);
	CHECK(room.member(room.spectator).spectatorLocked);
	CHECK(!room.ReadyBoth());
	CHECK(room.table().phase == TablePhase::Ready && room.table().spectatorHold);
	CHECK(!room.authority.BeginMatch(0, room.p1, room.p2).accepted);
	// Nothing else releases it early.
	CHECK(!StartsMatch(room.authority.AdvanceTime(5000)));
	CHECK(StartsMatch(room.authority.Apply(room.spectator, [&] {
		auto action = TableAction(room.authority, room.spectator, ActionKind::AcknowledgeTerminal);
		action.matchGeneration = room.finished;
		return action;
	}())));
	CHECK(!room.table().spectatorHold);
	CHECK(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
	CHECK((room.authority.MatchRoster(0) == std::vector<MemberId>{room.p1, room.p2, room.spectator}));
	// The lock lasts across games, and the watcher reads as watching.
	CHECK(room.member(room.spectator).spectatorLocked);
	CHECK(room.member(room.spectator).status == MemberStatus::Watching);
}

static void TestHoldExpires() {
	Room room(true);
	CHECK(!room.ReadyBoth());
	CHECK(!room.authority.HasDueTimerTransition(1000 + SpectatorStartHoldMs - 1));
	CHECK(!StartsMatch(room.authority.AdvanceTime(1000 + SpectatorStartHoldMs - 1)));
	CHECK(room.authority.HasDueTimerTransition(1000 + SpectatorStartHoldMs));
	CHECK(StartsMatch(room.authority.AdvanceTime(1000 + SpectatorStartHoldMs)));
	CHECK(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
	CHECK((room.authority.MatchRoster(0) == std::vector<MemberId>{room.p1, room.p2}));
	// Left out of this game, it waits for the next one, even once it is back.
	CHECK(room.member(room.spectator).status == MemberStatus::WatchingNext);
	CHECK(room.Acknowledge(room.spectator));
	CHECK(room.member(room.spectator).status == MemberStatus::WatchingNext);
}

static void TestUnlockedSpectatorNeverHolds() {
	Room room(false);
	CHECK(room.ReadyBoth());
	CHECK(!room.table().spectatorHold);
}

static void TestUnreadyEndsTheHoldAndUnwatchReleasesIt() {
	Room room(true);
	CHECK(!room.ReadyBoth());
	const auto unready = room.Apply(room.p1, ActionKind::Unready);
	CHECK(unready.accepted && !StartsMatch(unready));
	CHECK(room.table().phase == TablePhase::Waiting && !room.table().spectatorHold);
	CHECK(!StartsMatch(room.authority.AdvanceTime(1000 + SpectatorStartHoldMs)));
	const auto again = room.Apply(room.p1, ActionKind::Ready);
	CHECK(again.accepted && !StartsMatch(again) && room.table().spectatorHold);
	const auto unwatch = room.Apply(room.spectator, ActionKind::Unwatch);
	CHECK(unwatch.accepted && StartsMatch(unwatch));
	CHECK(!room.member(room.spectator).spectatorLocked);
}

static void TestUnlockAndLeaveReleaseTheHold() {
	Room unlocking(true);
	CHECK(!unlocking.ReadyBoth());
	CHECK(StartsMatch(unlocking.Lock(unlocking.spectator, false)));
	Room leaving(true);
	CHECK(!leaving.ReadyBoth());
	CHECK(StartsMatch(leaving.authority.Leave(leaving.spectator)));
}

static void TestOnlyWatchersLockIn() {
	Room room(false);
	const auto seated = room.Lock(room.p1, true);
	CHECK(!seated.accepted && seated.reason == RejectReason::NotWatching);
	const auto queued = room.Join("Queued", 4);
	CHECK(room.Apply(queued, ActionKind::Queue).accepted);
	CHECK(room.ReadyBoth());
	CHECK(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
	// A queued member watches the game it waits out, but not by choice.
	const auto waiting = room.Lock(queued, true);
	CHECK(!waiting.accepted && waiting.reason == RejectReason::NotWatching);
	// A spectator can lock in during a game, receipt or not.
	CHECK(room.Lock(room.spectator, true).accepted && room.member(room.spectator).spectatorLocked);
	auto action = TableAction(room.authority, room.spectator, ActionKind::LockSpectating);
	action.locked = true;
	Action decoded;
	nlohmann::json(action).get_to(decoded);
	CHECK(decoded.kind == ActionKind::LockSpectating && decoded.locked);
}

static void TestLockStaysWithItsTable() {
	Room room(true);
	CHECK(room.Acknowledge(room.spectator));
	// Moving straight to another table must not carry the lock there.
	auto watchOther = TableAction(room.authority, room.spectator, ActionKind::Watch);
	watchOther.table = 1;
	watchOther.tableRevision = room.authority.SnapshotView().tables[1].revision;
	CHECK(room.authority.Apply(room.spectator, watchOther).accepted);
	CHECK(!room.member(room.spectator).spectatorLocked);
}

static void TestHoldSurvivesRecovery() {
	Room room(true);
	CHECK(!room.ReadyBoth());
	room.authority.AdvanceTime(5000);
	RoomAuthority replica("Replica");
	CHECK(replica.RestoreCheckpoint(nlohmann::json::parse(room.authority.Checkpoint().dump())));
	CHECK(replica.Checkpoint() == room.authority.Checkpoint());
	room.authority.PauseForRecovery();
	const auto paused = room.authority.Checkpoint();
	CHECK(paused.at("hold_age").at(0).get<std::uint64_t>() == 4000);
	RoomAuthority follower("Follower");
	CHECK(follower.RestoreCheckpoint(paused));
	CHECK(follower.SnapshotView().tables[0].spectatorHold && follower.SnapshotView().members.back().spectatorLocked);
	follower.AdvancePausedTimers(SpectatorStartHoldMs - 4000);
	// The receiving clock starts lower; the hold age carries over.
	CHECK(follower.HasDueTimerTransition(0));
	CHECK(StartsMatch(follower.AdvanceTime(0)));

	// Older checkpoints and snapshots have none of the new fields.
	auto legacy = paused;
	legacy.erase("hold_age");
	legacy["snapshot"]["tables"][0].erase("spectator_hold");
	for (auto& member : legacy["snapshot"]["members"]) member.erase("spectator_locked");
	RoomAuthority old("Old");
	CHECK(old.RestoreCheckpoint(legacy));
	CHECK(!old.SnapshotView().tables[0].spectatorHold);
}

int main() {
	TestAcknowledgementReleasesTheHold();
	TestHoldExpires();
	TestUnlockedSpectatorNeverHolds();
	TestUnreadyEndsTheHoldAndUnwatchReleasesIt();
	TestUnlockAndLeaveReleaseTheHold();
	TestOnlyWatchersLockIn();
	TestLockStaysWithItsTable();
	TestHoldSurvivesRecovery();
	if (failures) return 1;
	std::puts("RoomSpectatorLock test passed");
	return 0;
}
