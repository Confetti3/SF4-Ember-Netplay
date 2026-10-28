// A locked-in spectator holds the next start at its table for a bounded time
// while it retires the previous game.
#include "../netplay/MatchEndRules.hxx"
#include "../session/RoomModel.hxx"

#include <algorithm>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>
#include "test_support.hxx"

using namespace sf4e::room;

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
		for (const auto member : {p1, p2}) EXPECT(Apply(member, ActionKind::Queue).accepted);
		EXPECT(Apply(spectator, ActionKind::Watch).accepted);
		if (locked) EXPECT(Lock(spectator, true).accepted);
		EXPECT(ReadyBoth());
		EXPECT(authority.BeginMatch(0, p1, p2).accepted);
		finished = authority.SnapshotView().tables[0].matchGeneration;
		EXPECT(authority.EndMatch(0, finished, MatchResult::P1Win).accepted);
		EXPECT(Acknowledge(p1) && Acknowledge(p2));
		EXPECT(authority.SnapshotFor(spectator).localTerminalPending);
	}
	MemberId Join(const char* name, int index, bool host = false) {
		const auto result = authority.Join(name, ConnectionRef{"host", std::to_string(index)}, host);
		EXPECT(result.accepted);
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
		EXPECT(Apply(p1, ActionKind::Ready).accepted);
		const auto second = Apply(p2, ActionKind::Ready);
		EXPECT(second.accepted);
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
	EXPECT(room.member(room.spectator).spectatorLocked);
	EXPECT(!room.ReadyBoth());
	EXPECT(room.table().phase == TablePhase::Ready && room.table().spectatorHold);
	EXPECT(!room.authority.BeginMatch(0, room.p1, room.p2).accepted);
	// Nothing else releases it early.
	EXPECT(!StartsMatch(room.authority.AdvanceTime(5000)));
	EXPECT(StartsMatch(room.authority.Apply(room.spectator, [&] {
		auto action = TableAction(room.authority, room.spectator, ActionKind::AcknowledgeTerminal);
		action.matchGeneration = room.finished;
		return action;
	}())));
	EXPECT(!room.table().spectatorHold);
	EXPECT(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
	EXPECT((room.authority.MatchRoster(0) == std::vector<MemberId>{room.p1, room.p2, room.spectator}));
	// The lock lasts across games, and the watcher reads as watching.
	EXPECT(room.member(room.spectator).spectatorLocked);
	EXPECT(room.member(room.spectator).status == MemberStatus::Watching);
}

static void TestHoldExpires() {
	Room room(true);
	EXPECT(!room.ReadyBoth());
	EXPECT(!room.authority.HasDueTimerTransition(1000 + SpectatorStartHoldMs - 1));
	EXPECT(!StartsMatch(room.authority.AdvanceTime(1000 + SpectatorStartHoldMs - 1)));
	EXPECT(room.authority.HasDueTimerTransition(1000 + SpectatorStartHoldMs));
	EXPECT(StartsMatch(room.authority.AdvanceTime(1000 + SpectatorStartHoldMs)));
	EXPECT(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
	EXPECT((room.authority.MatchRoster(0) == std::vector<MemberId>{room.p1, room.p2}));
	// Left out of this game, it waits for the next one, even once it is back.
	EXPECT(room.member(room.spectator).status == MemberStatus::WatchingNext);
	EXPECT(room.Acknowledge(room.spectator));
	EXPECT(room.member(room.spectator).status == MemberStatus::WatchingNext);
}

static void TestUnlockedSpectatorNeverHolds() {
	Room room(false);
	EXPECT(room.ReadyBoth());
	EXPECT(!room.table().spectatorHold);
}

static void TestUnreadyEndsTheHoldAndUnwatchReleasesIt() {
	Room room(true);
	EXPECT(!room.ReadyBoth());
	const auto unready = room.Apply(room.p1, ActionKind::Unready);
	EXPECT(unready.accepted && !StartsMatch(unready));
	EXPECT(room.table().phase == TablePhase::Waiting && !room.table().spectatorHold);
	EXPECT(!StartsMatch(room.authority.AdvanceTime(1000 + SpectatorStartHoldMs)));
	const auto again = room.Apply(room.p1, ActionKind::Ready);
	EXPECT(again.accepted && !StartsMatch(again) && room.table().spectatorHold);
	const auto unwatch = room.Apply(room.spectator, ActionKind::Unwatch);
	EXPECT(unwatch.accepted && StartsMatch(unwatch));
	EXPECT(!room.member(room.spectator).spectatorLocked);
}

static void TestUnlockAndLeaveReleaseTheHold() {
	Room unlocking(true);
	EXPECT(!unlocking.ReadyBoth());
	EXPECT(StartsMatch(unlocking.Lock(unlocking.spectator, false)));
	Room leaving(true);
	EXPECT(!leaving.ReadyBoth());
	EXPECT(StartsMatch(leaving.authority.Leave(leaving.spectator)));
}

static void TestOnlyWatchersLockIn() {
	Room room(false);
	const auto seated = room.Lock(room.p1, true);
	EXPECT(!seated.accepted && seated.reason == RejectReason::NotWatching);
	const auto queued = room.Join("Queued", 4);
	EXPECT(room.Apply(queued, ActionKind::Queue).accepted);
	EXPECT(room.ReadyBoth());
	EXPECT(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
	// A queued member watches the game it waits out, but not by choice.
	const auto waiting = room.Lock(queued, true);
	EXPECT(!waiting.accepted && waiting.reason == RejectReason::NotWatching);
	// A spectator can lock in during a game, receipt or not.
	EXPECT(room.Lock(room.spectator, true).accepted && room.member(room.spectator).spectatorLocked);
	auto action = TableAction(room.authority, room.spectator, ActionKind::LockSpectating);
	action.locked = true;
	Action decoded;
	nlohmann::json(action).get_to(decoded);
	EXPECT(decoded.kind == ActionKind::LockSpectating && decoded.locked);
}

static void TestLockStaysWithItsTable() {
	Room room(true);
	EXPECT(room.Acknowledge(room.spectator));
	// Moving straight to another table must not carry the lock there.
	auto watchOther = TableAction(room.authority, room.spectator, ActionKind::Watch);
	watchOther.table = 1;
	watchOther.tableRevision = room.authority.SnapshotView().tables[1].revision;
	EXPECT(room.authority.Apply(room.spectator, watchOther).accepted);
	EXPECT(!room.member(room.spectator).spectatorLocked);
}

// A fighter who leaves, or is kicked, during the start hold ends no game: the
// table is in Ready, and the generation it still names finished with a real
// result that the spectator has yet to acknowledge.
static void TestFighterDepartureDuringHold() {
	for (const bool kicked : {false, true}) {
		Room room(true);
		EXPECT(!room.ReadyBoth());
		EXPECT(room.table().spectatorHold);
		Result departure;
		if (kicked) {
			const auto host = room.authority.SnapshotView().host;
			auto kick = TableAction(room.authority, host, ActionKind::Kick);
			kick.target = room.p2;
			departure = room.authority.Apply(host, kick);
		} else departure = room.authority.Leave(room.p2);
		EXPECT(departure.accepted);
		EXPECT(std::none_of(departure.events.begin(), departure.events.end(),
			[](const Event& event) { return event.kind == Event::Kind::MatchEnded; }));
		EXPECT(!StartsMatch(departure));
		EXPECT(!room.table().spectatorHold && room.table().phase != TablePhase::Ready);
		const auto pending = room.authority.PendingTerminalEvents(room.spectator);
		EXPECT(pending.size() == 1 && pending[0].generation == room.finished && pending[0].result == MatchResult::P1Win);
		EXPECT(room.Acknowledge(room.spectator));
	}
}

// A live game with the spectator granted into it: the roster, generation and
// the spectator's lock-in are all in place.
struct LiveGame {
	Room room;
	std::uint64_t generation = 0;
	explicit LiveGame(bool locked) : room(locked) {
		EXPECT(room.Acknowledge(room.spectator));
		EXPECT(room.ReadyBoth());
		EXPECT(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
		generation = room.table().matchGeneration;
		EXPECT((room.authority.MatchRoster(0) == std::vector<MemberId>{room.p1, room.p2, room.spectator}));
	}
	Result Unwatch(bool keepWatching) {
		auto action = TableAction(room.authority, room.spectator, ActionKind::Unwatch);
		action.matchGeneration = generation;
		action.tableRevision = 0; // a generation-scoped action skips the table revision fence
		action.keepWatching = keepWatching;
		return room.authority.Apply(room.spectator, action);
	}
};

// A spectator whose own stream or setup failed leaves that game only: it stays
// on the table for later games, without its lock-in, so it cannot hold a start.
static void TestStreamLossKeepsTheWatch() {
	LiveGame game(true);
	EXPECT(game.room.member(game.room.spectator).spectatorLocked);
	const auto lost = game.Unwatch(true);
	EXPECT(lost.accepted);
	const auto& spectators = game.room.table().spectators;
	EXPECT(std::find(spectators.begin(), spectators.end(), game.room.spectator) != spectators.end());
	EXPECT(!game.room.member(game.room.spectator).spectatorLocked);
	// The game ends, the fighters ready again, and nothing waits for it.
	EXPECT(game.room.authority.EndMatch(0, game.generation, MatchResult::Abort).accepted);
	game.room.finished = game.generation;
	EXPECT(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	EXPECT(game.room.ReadyBoth() && !game.room.table().spectatorHold);
	EXPECT(game.room.Acknowledge(game.room.spectator));
	EXPECT(game.room.authority.BeginMatch(0, game.room.p1, game.room.p2).accepted);
	EXPECT((game.room.authority.MatchRoster(0) == std::vector<MemberId>{game.room.p1, game.room.p2, game.room.spectator}));

	// A player's own Stop watching sends no such flag, and leaves the table.
	LiveGame stopped(true);
	EXPECT(stopped.Unwatch(false).accepted);
	const auto& left = stopped.room.table().spectators;
	EXPECT(std::find(left.begin(), left.end(), stopped.room.spectator) == left.end());
	EXPECT(!stopped.room.member(stopped.room.spectator).spectatorLocked);

	// Only a member of the live roster has a game to leave.
	LiveGame outsider(false);
	const auto stranger = outsider.room.Join("Stranger", 9);
	auto action = TableAction(outsider.room.authority, stranger, ActionKind::Unwatch);
	action.matchGeneration = outsider.generation; action.keepWatching = true;
	EXPECT(!outsider.room.authority.Apply(stranger, action).accepted);
	// A fighter is not a spectator either.
	action = TableAction(outsider.room.authority, outsider.room.p1, ActionKind::Unwatch);
	action.matchGeneration = outsider.generation; action.keepWatching = true;
	EXPECT(!outsider.room.authority.Apply(outsider.room.p1, action).accepted);

	// The flag survives the wire, and an action without it decodes to false.
	Action keep = TableAction(game.room.authority, game.room.spectator, ActionKind::Unwatch);
	keep.keepWatching = true;
	Action decoded;
	nlohmann::json(keep).get_to(decoded);
	EXPECT(decoded.keepWatching);
	auto legacy = nlohmann::json(keep);
	legacy.erase("keep_watching");
	legacy.get_to(decoded);
	EXPECT(!decoded.keepWatching);
	legacy["keep_watching"] = 1;
	bool rejected = false;
	try { legacy.get_to(decoded); } catch (const std::exception&) { rejected = true; }
	EXPECT(rejected);
}

// What a locked-in spectator's runtime does around its own stream failure. The
// failure is recorded when it happens and arms a lock release that runs apart
// from the battle's close; the sessionless close rule only decides how the view
// ends. Times are in milliseconds and only matter to the release's expiry.
using sf4e::netplay::SessionlessClose;
using sf4e::netplay::SpectatorLockRelease;

static SessionlessClose SpectatorBattleClosed(const LiveGame& game, bool endCommitted, bool streamFailed) {
	return sf4e::netplay::SessionlessCloseAction(true, game.generation, game.generation, game.generation,
		false, true, true, endCommitted, streamFailed);
}

static void ArmRelease(SpectatorLockRelease& release, const LiveGame& game) {
	release.Arm(0, game.generation, game.room.authority.SnapshotView().roomEpoch, 1000);
}

// The release goes out under an action id above the ones TableAction makes, so
// a later action of the same member has to outrank it.
static std::uint64_t NextActionId() {
	static std::uint64_t next = 900000;
	return next++;
}

// One tick of the runtime's release, through the calls production makes: send
// what the projection asks for, record that it was queued, and pass an accepted
// reply back. Queueing only reaches the local control link, so delivered=false
// models the helper refusing a queued action (no reply ever comes), and
// replyArrives=false models a reply lost after the authority took the action.
// True when the authority accepted a release.
static bool TickRelease(SpectatorLockRelease& release, LiveGame& game, std::uint64_t nowMs = 1500,
	bool delivered = true, bool replyArrives = true) {
	Action action;
	const auto snapshot = game.room.authority.SnapshotFor(game.room.spectator);
	if (release.Next(snapshot, nowMs, &action) != SpectatorLockRelease::Step::Send) return false;
	action.actionId = NextActionId();
	release.Queued(action.actionId, nowMs);
	if (!delivered) return false;
	const auto result = game.room.authority.Apply(game.room.spectator, action);
	if (result.accepted && replyArrives) release.Acknowledged(action.actionId);
	return result.accepted;
}

static bool Watches(const LiveGame& game) {
	const auto& spectators = game.room.table().spectators;
	return std::find(spectators.begin(), spectators.end(), game.room.spectator) != spectators.end();
}

// Failure recorded, then the result committed, then the native close: the
// lock is released and the watch kept. The generation-scoped Unwatch that used
// to carry this is refused once the result has moved the table on.
static void TestGgpoFailureDropsTheLock() {
	LiveGame failed(true);
	EXPECT(failed.room.member(failed.room.spectator).spectatorLocked);
	SpectatorLockRelease release;
	ArmRelease(release, failed);
	EXPECT(SpectatorBattleClosed(failed, false, true) == SessionlessClose::LeaveGame);
	EXPECT(failed.room.authority.EndMatch(0, failed.generation, MatchResult::P1Win).accepted);
	failed.room.finished = failed.generation;
	EXPECT(SpectatorBattleClosed(failed, true, true) == SessionlessClose::LeaveGame);
	EXPECT(!failed.Unwatch(true).accepted);
	EXPECT(failed.room.member(failed.room.spectator).spectatorLocked);
	EXPECT(TickRelease(release, failed));
	EXPECT(!release.Pending());
	EXPECT(Watches(failed) && !failed.room.member(failed.room.spectator).spectatorLocked);
	EXPECT(failed.room.Acknowledge(failed.room.p1) && failed.room.Acknowledge(failed.room.p2));
	EXPECT(failed.room.ReadyBoth() && !failed.room.table().spectatorHold);

	// The same release with the close first and the result after it.
	LiveGame closedFirst(true);
	ArmRelease(release, closedFirst);
	EXPECT(SpectatorBattleClosed(closedFirst, false, true) == SessionlessClose::LeaveGame);
	EXPECT(TickRelease(release, closedFirst));
	EXPECT(Watches(closedFirst) && !closedFirst.room.member(closedFirst.room.spectator).spectatorLocked);
	EXPECT(closedFirst.room.authority.EndMatch(0, closedFirst.generation, MatchResult::P1Win).accepted);
	EXPECT(Watches(closedFirst));

	// A spectator that merely played out or fell behind a game the players
	// finished has no failure to record: nothing is armed or sent, so its
	// lock-in stands for the next start.
	LiveGame behind(true);
	EXPECT(SpectatorBattleClosed(behind, false, false) == SessionlessClose::EndView);
	EXPECT(SpectatorBattleClosed(behind, true, false) == SessionlessClose::EndView);
	SpectatorLockRelease unarmed;
	EXPECT(!TickRelease(unarmed, behind));
	EXPECT(behind.room.member(behind.room.spectator).spectatorLocked);
	EXPECT(behind.room.authority.EndMatch(0, behind.generation, MatchResult::P1Win).accepted);
	behind.room.finished = behind.generation;
	EXPECT(behind.room.Acknowledge(behind.room.p1) && behind.room.Acknowledge(behind.room.p2));
	EXPECT(!behind.room.ReadyBoth() && behind.room.table().spectatorHold);
}

// A spectator whose gameplay connection fails during setup has no GGPO session
// and no entered battle, so no stream failure is ever recorded. The runtime arms
// the release from the admitted generation at the failure itself. The Unwatch it
// sends for the same failure is queued once and can be lost with the replaced
// control, so it is not what clears the lock.
static void TestSetupFailureBeforeGgpoDropsTheLock() {
	using sf4e::netplay::SpectatorFailureOwesLockRelease;
	LiveGame game(true);
	EXPECT(game.room.member(game.room.spectator).spectatorLocked);
	const auto& table = game.room.table();
	const bool finished = sf4e::netplay::GenerationEnded(table, game.generation);
	EXPECT(!finished);
	EXPECT(SpectatorFailureOwesLockRelease(true, game.generation, finished));
	SpectatorLockRelease release;
	ArmRelease(release, game);
	// The Unwatch was queued and lost with the old control: no reply, nothing applied.
	EXPECT(game.room.member(game.room.spectator).spectatorLocked);
	// The result commits before the release is sent, which refuses the Unwatch.
	EXPECT(game.room.authority.EndMatch(0, game.generation, MatchResult::P1Win).accepted);
	game.room.finished = game.generation;
	EXPECT(!game.Unwatch(true).accepted);
	// The first release copy is lost as well; the resend reaches the recovered control.
	EXPECT(!TickRelease(release, game, 1500, false));
	EXPECT(release.Pending() && game.room.member(game.room.spectator).spectatorLocked);
	EXPECT(!TickRelease(release, game, 1500 + SpectatorLockRelease::RetryMs - 1));
	EXPECT(TickRelease(release, game, 1500 + SpectatorLockRelease::RetryMs));
	EXPECT(!release.Pending());
	EXPECT(Watches(game) && !game.room.member(game.room.spectator).spectatorLocked);
	EXPECT(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	EXPECT(game.room.ReadyBoth() && !game.room.table().spectatorHold);

	// The same failure with the result committed before the tick that records it
	// needs nothing, and neither does a fighter or a grant that named no generation.
	EXPECT(!SpectatorFailureOwesLockRelease(true, game.generation, true));
	EXPECT(!SpectatorFailureOwesLockRelease(false, game.generation, false));
	EXPECT(!SpectatorFailureOwesLockRelease(true, 0, false));

	// A later explicit lock-in wins over the release the setup failure armed.
	LiveGame relocked(true);
	ArmRelease(release, relocked);
	EXPECT(!TickRelease(release, relocked, 1500, false));
	Action press = TableAction(relocked.room.authority, relocked.room.spectator, ActionKind::LockSpectating);
	press.locked = true;
	release.Observe(press);
	EXPECT(!release.Pending());
	EXPECT(!TickRelease(release, relocked, 1500 + SpectatorLockRelease::RetryMs));
	EXPECT(relocked.room.member(relocked.room.spectator).spectatorLocked);

	// A spectator that merely fell behind a game the room finished records no
	// failure: the rule owes nothing, the close ends the view silently, and its
	// lock-in stands.
	LiveGame behind(true);
	EXPECT(behind.room.authority.EndMatch(0, behind.generation, MatchResult::P1Win).accepted);
	behind.room.finished = behind.generation;
	EXPECT(!SpectatorFailureOwesLockRelease(true, behind.generation,
		sf4e::netplay::GenerationEnded(behind.room.table(), behind.generation)));
	EXPECT(SpectatorBattleClosed(behind, true, false) == SessionlessClose::EndView);
	SpectatorLockRelease unarmed;
	EXPECT(!TickRelease(unarmed, behind, 1500 + SpectatorLockRelease::RetryMs));
	EXPECT(behind.room.member(behind.room.spectator).spectatorLocked);
}

// A lock-in the player makes after the failure is newer than the release, and
// a release still waiting to be sent must not undo it.
static void TestLaterLockInSurvivesStaleRelease() {
	// Contrast: without the cancel, the stale release clears a lock-in made after it was armed.
	LiveGame stale(false);
	SpectatorLockRelease release;
	ArmRelease(release, stale);
	EXPECT(stale.room.authority.EndMatch(0, stale.generation, MatchResult::P1Win).accepted);
	EXPECT(stale.room.Lock(stale.room.spectator, true).accepted);
	EXPECT(TickRelease(release, stale));
	EXPECT(!stale.room.member(stale.room.spectator).spectatorLocked);

	LiveGame game(false);
	ArmRelease(release, game);
	EXPECT(game.room.authority.EndMatch(0, game.generation, MatchResult::P1Win).accepted);
	game.room.finished = game.generation;
	Action press = TableAction(game.room.authority, game.room.spectator, ActionKind::LockSpectating);
	press.locked = true;
	release.Observe(press);
	EXPECT(game.room.authority.Apply(game.room.spectator, press).accepted);
	EXPECT(!TickRelease(release, game));
	EXPECT(!release.Pending());
	EXPECT(game.room.member(game.room.spectator).spectatorLocked);
	EXPECT(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	EXPECT(!game.room.ReadyBoth() && game.room.table().spectatorHold);

	// A release that was already sent stays ordered before a later lock-in.
	LiveGame ordered(true);
	ArmRelease(release, ordered);
	EXPECT(TickRelease(release, ordered));
	EXPECT(!ordered.room.member(ordered.room.spectator).spectatorLocked);
	EXPECT(ordered.room.authority.EndMatch(0, ordered.generation, MatchResult::P1Win).accepted);
	auto lockIn = TableAction(ordered.room.authority, ordered.room.spectator, ActionKind::LockSpectating);
	lockIn.locked = true; lockIn.actionId = NextActionId();
	EXPECT(ordered.room.authority.Apply(ordered.room.spectator, lockIn).accepted);
	EXPECT(ordered.room.member(ordered.room.spectator).spectatorLocked && Watches(ordered));
}

// The helper can refuse a release the runtime queued (the control was replaced
// or its queue was full). Queueing therefore does not finish it: it is sent
// again, after the match result has committed, until the authority takes it.
static void TestQueuedButUndeliveredReleaseIsResent() {
	using Step = SpectatorLockRelease::Step;
	LiveGame game(true);
	SpectatorLockRelease release;
	ArmRelease(release, game);
	EXPECT(game.room.authority.EndMatch(0, game.generation, MatchResult::P1Win).accepted);
	game.room.finished = game.generation;
	// Queued, then lost with the old control: no reply, the lock is still on.
	EXPECT(!TickRelease(release, game, 1500, false));
	EXPECT(release.Pending() && game.room.member(game.room.spectator).spectatorLocked);
	// It waits for the authority before sending again.
	Action action;
	const auto snapshot = game.room.authority.SnapshotFor(game.room.spectator);
	EXPECT(release.Next(snapshot, 1500 + SpectatorLockRelease::RetryMs - 1, &action) == Step::Await);
	// The second copy is lost too; the third reaches the recovered control.
	EXPECT(!TickRelease(release, game, 1500 + SpectatorLockRelease::RetryMs, false));
	EXPECT(release.Pending() && game.room.member(game.room.spectator).spectatorLocked);
	EXPECT(TickRelease(release, game, 1500 + 2 * SpectatorLockRelease::RetryMs));
	EXPECT(!release.Pending());
	EXPECT(Watches(game) && !game.room.member(game.room.spectator).spectatorLocked);
	EXPECT(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	EXPECT(game.room.ReadyBoth() && !game.room.table().spectatorHold);

	// The authority took the release but its reply was lost: the projection
	// showing the member unlocked ends it, and nothing is sent again.
	LiveGame silent(true);
	ArmRelease(release, silent);
	EXPECT(TickRelease(release, silent, 1500, true, false));
	EXPECT(release.Pending());
	const auto unlocked = silent.room.authority.SnapshotFor(silent.room.spectator);
	EXPECT(release.Next(unlocked, 1500 + SpectatorLockRelease::RetryMs, &action) == Step::Done);
	EXPECT(!TickRelease(release, silent, 1500 + SpectatorLockRelease::RetryMs));

	// Undelivered until it lapses: it is given up, and the lock stays.
	LiveGame lapsed(true);
	ArmRelease(release, lapsed);
	EXPECT(!TickRelease(release, lapsed, 1500, false));
	EXPECT(release.Next(lapsed.room.authority.SnapshotFor(lapsed.room.spectator),
		1000 + SpectatorLockRelease::ExpiryMs, &action) == Step::Drop);
}

// A lock-in the player makes after the failure survives whatever the release
// does around it: a queued copy that was delivered but not answered, one still
// in flight, and the resends that would have followed.
static void TestLockInAfterFailureSurvivesResends() {
	LiveGame game(true);
	SpectatorLockRelease release;
	ArmRelease(release, game);
	EXPECT(game.room.authority.EndMatch(0, game.generation, MatchResult::P1Win).accepted);
	game.room.finished = game.generation;
	// The release lands but its reply is lost, so it is still pending.
	EXPECT(TickRelease(release, game, 1500, true, false));
	EXPECT(release.Pending() && !game.room.member(game.room.spectator).spectatorLocked);
	// The player locks in again, the way the runtime dispatches the press.
	Action press = TableAction(game.room.authority, game.room.spectator, ActionKind::LockSpectating);
	press.locked = true;
	release.Observe(press);
	press.actionId = NextActionId();
	EXPECT(game.room.authority.Apply(game.room.spectator, press).accepted);
	EXPECT(game.room.member(game.room.spectator).spectatorLocked);
	// No resend, however long it has been.
	for (std::uint64_t now = 2500; now < 1000 + SpectatorLockRelease::ExpiryMs; now += SpectatorLockRelease::RetryMs)
		EXPECT(!TickRelease(release, game, now));
	EXPECT(!release.Pending() && game.room.member(game.room.spectator).spectatorLocked);
	EXPECT(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	EXPECT(!game.room.ReadyBoth() && game.room.table().spectatorHold);

	// A copy still in flight when the player locks in is older than the lock-in
	// and is refused by the member's action watermark if it lands late.
	LiveGame flight(true);
	ArmRelease(release, flight);
	Action held;
	EXPECT(release.Next(flight.room.authority.SnapshotFor(flight.room.spectator), 1500, &held) == SpectatorLockRelease::Step::Send);
	held.actionId = NextActionId();
	release.Queued(held.actionId, 1500);
	press = TableAction(flight.room.authority, flight.room.spectator, ActionKind::LockSpectating);
	press.locked = true;
	release.Observe(press);
	press.actionId = NextActionId();
	EXPECT(flight.room.authority.Apply(flight.room.spectator, press).accepted);
	EXPECT(!flight.room.authority.Apply(flight.room.spectator, held).accepted);
	EXPECT(!TickRelease(release, flight, 1500 + SpectatorLockRelease::RetryMs));
	EXPECT(flight.room.member(flight.room.spectator).spectatorLocked);
}

static void TestHoldSurvivesRecovery() {
	Room room(true);
	EXPECT(!room.ReadyBoth());
	room.authority.AdvanceTime(5000);
	RoomAuthority replica("Replica");
	EXPECT(replica.RestoreCheckpoint(nlohmann::json::parse(room.authority.Checkpoint().dump())));
	EXPECT(replica.Checkpoint() == room.authority.Checkpoint());
	room.authority.PauseForRecovery();
	const auto paused = room.authority.Checkpoint();
	EXPECT(paused.at("hold_age").at(0).get<std::uint64_t>() == 4000);
	RoomAuthority follower("Follower");
	EXPECT(follower.RestoreCheckpoint(paused));
	EXPECT(follower.SnapshotView().tables[0].spectatorHold && follower.SnapshotView().members.back().spectatorLocked);
	follower.AdvancePausedTimers(SpectatorStartHoldMs - 4000);
	// The receiving clock starts lower; the hold age carries over.
	EXPECT(follower.HasDueTimerTransition(0));
	EXPECT(StartsMatch(follower.AdvanceTime(0)));

	// Checkpoints and snapshots written without these fields load with defaults.
	auto legacy = paused;
	legacy.erase("hold_age");
	legacy["snapshot"]["tables"][0].erase("spectator_hold");
	for (auto& member : legacy["snapshot"]["members"]) member.erase("spectator_locked");
	RoomAuthority old("Old");
	EXPECT(old.RestoreCheckpoint(legacy));
	EXPECT(!old.SnapshotView().tables[0].spectatorHold);
}

// A held table's sent snapshot says how long the hold has left, so a client
// can count it down. The room's own state and its checkpoints never carry it,
// and a snapshot from a host that does not send it reads as zero.
static void TestHoldTimeLeftIsStampedWhenSent() {
	Room room(true);
	EXPECT(!room.ReadyBoth());
	EXPECT(room.authority.SnapshotFor(room.p1).tables[0].holdRemainingMs == SpectatorStartHoldMs);
	EXPECT(room.authority.SnapshotFor(room.spectator).tables[0].holdRemainingMs == SpectatorStartHoldMs);
	EXPECT(room.authority.SnapshotView().tables[0].holdRemainingMs == 0);
	EXPECT(room.authority.SnapshotFor(room.p1).tables[1].holdRemainingMs == 0);
	room.authority.AdvanceTime(5000);
	const auto sent = room.authority.SnapshotFor(room.p2);
	EXPECT(sent.tables[0].holdRemainingMs == SpectatorStartHoldMs - 4000);
	const auto wire = nlohmann::json(sent);
	EXPECT(wire.at("tables").at(0).at("hold_ms") == SpectatorStartHoldMs - 4000);
	EXPECT(!wire.at("tables").at(1).contains("hold_ms"));
	EXPECT(wire.get<Snapshot>().tables[0].holdRemainingMs == SpectatorStartHoldMs - 4000);
	EXPECT(!nlohmann::json(room.authority.SnapshotView()).at("tables").at(0).contains("hold_ms"));
	EXPECT(room.authority.Checkpoint().dump().find("hold_ms") == std::string::npos);
	// An older host sends none; out of range is refused.
	auto legacy = wire;
	legacy["tables"][0].erase("hold_ms");
	EXPECT(legacy.get<Snapshot>().tables[0].holdRemainingMs == 0 && legacy.get<Snapshot>().tables[0].spectatorHold);
	auto wrong = wire;
	wrong["tables"][0]["hold_ms"] = SpectatorStartHoldMs + 1;
	bool threw = false;
	try { wrong.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	EXPECT(threw);
	// Paused for recovery, the hold keeps its age.
	room.authority.PauseForRecovery();
	EXPECT(room.authority.SnapshotFor(room.p1).tables[0].holdRemainingMs == SpectatorStartHoldMs - 4000);
	room.authority.AdvanceTime(5000);
	// Due but not yet released: still at least 1, never zero while held.
	room.authority.AdvanceTime(1000 + SpectatorStartHoldMs - 1);
	EXPECT(room.authority.SnapshotFor(room.p1).tables[0].holdRemainingMs == 1);
	EXPECT(StartsMatch(room.authority.AdvanceTime(1000 + SpectatorStartHoldMs)));
	EXPECT(room.authority.SnapshotFor(room.p1).tables[0].holdRemainingMs == 0);
}

int main() {
	TestHoldTimeLeftIsStampedWhenSent();
	TestAcknowledgementReleasesTheHold();
	TestHoldExpires();
	TestUnlockedSpectatorNeverHolds();
	TestUnreadyEndsTheHoldAndUnwatchReleasesIt();
	TestUnlockAndLeaveReleaseTheHold();
	TestOnlyWatchersLockIn();
	TestLockStaysWithItsTable();
	TestFighterDepartureDuringHold();
	TestStreamLossKeepsTheWatch();
	TestGgpoFailureDropsTheLock();
	TestSetupFailureBeforeGgpoDropsTheLock();
	TestLaterLockInSurvivesStaleRelease();
	TestQueuedButUndeliveredReleaseIsResent();
	TestLockInAfterFailureSurvivesResends();
	TestHoldSurvivesRecovery();
	if (TEST_FAILURES()) return 1;
	std::puts("RoomSpectatorLock test passed");
	return 0;
}
