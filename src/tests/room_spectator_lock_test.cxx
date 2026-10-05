// A locked-in spectator holds the next start at its table for a bounded time
// while it retires the previous game.
#include "../netplay/MatchEndRules.hxx"
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

// A fighter who leaves, or is kicked, during the start hold ends no game: the
// table is in Ready, and the generation it still names finished with a real
// result that the spectator has yet to acknowledge.
static void TestFighterDepartureDuringHold() {
	for (const bool kicked : {false, true}) {
		Room room(true);
		CHECK(!room.ReadyBoth());
		CHECK(room.table().spectatorHold);
		Result departure;
		if (kicked) {
			const auto host = room.authority.SnapshotView().host;
			auto kick = TableAction(room.authority, host, ActionKind::Kick);
			kick.target = room.p2;
			departure = room.authority.Apply(host, kick);
		} else departure = room.authority.Leave(room.p2);
		CHECK(departure.accepted);
		CHECK(std::none_of(departure.events.begin(), departure.events.end(),
			[](const Event& event) { return event.kind == Event::Kind::MatchEnded; }));
		CHECK(!StartsMatch(departure));
		CHECK(!room.table().spectatorHold && room.table().phase != TablePhase::Ready);
		const auto pending = room.authority.PendingTerminalEvents(room.spectator);
		CHECK(pending.size() == 1 && pending[0].generation == room.finished && pending[0].result == MatchResult::P1Win);
		CHECK(room.Acknowledge(room.spectator));
	}
}

// A live game with the spectator granted into it: the roster, generation and
// the spectator's lock-in are all in place.
struct LiveGame {
	Room room;
	std::uint64_t generation = 0;
	explicit LiveGame(bool locked) : room(locked) {
		CHECK(room.Acknowledge(room.spectator));
		CHECK(room.ReadyBoth());
		CHECK(room.authority.BeginMatch(0, room.p1, room.p2).accepted);
		generation = room.table().matchGeneration;
		CHECK((room.authority.MatchRoster(0) == std::vector<MemberId>{room.p1, room.p2, room.spectator}));
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
	CHECK(game.room.member(game.room.spectator).spectatorLocked);
	const auto lost = game.Unwatch(true);
	CHECK(lost.accepted);
	const auto& spectators = game.room.table().spectators;
	CHECK(std::find(spectators.begin(), spectators.end(), game.room.spectator) != spectators.end());
	CHECK(!game.room.member(game.room.spectator).spectatorLocked);
	// The game ends, the fighters ready again, and nothing waits for it.
	CHECK(game.room.authority.EndMatch(0, game.generation, MatchResult::Abort).accepted);
	game.room.finished = game.generation;
	CHECK(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	CHECK(game.room.ReadyBoth() && !game.room.table().spectatorHold);
	CHECK(game.room.Acknowledge(game.room.spectator));
	CHECK(game.room.authority.BeginMatch(0, game.room.p1, game.room.p2).accepted);
	CHECK((game.room.authority.MatchRoster(0) == std::vector<MemberId>{game.room.p1, game.room.p2, game.room.spectator}));

	// A player's own Stop watching sends no such flag, and leaves the table.
	LiveGame stopped(true);
	CHECK(stopped.Unwatch(false).accepted);
	const auto& left = stopped.room.table().spectators;
	CHECK(std::find(left.begin(), left.end(), stopped.room.spectator) == left.end());
	CHECK(!stopped.room.member(stopped.room.spectator).spectatorLocked);

	// Only a member of the live roster has a game to leave.
	LiveGame outsider(false);
	const auto stranger = outsider.room.Join("Stranger", 9);
	auto action = TableAction(outsider.room.authority, stranger, ActionKind::Unwatch);
	action.matchGeneration = outsider.generation; action.keepWatching = true;
	CHECK(!outsider.room.authority.Apply(stranger, action).accepted);
	// A fighter is not a spectator either.
	action = TableAction(outsider.room.authority, outsider.room.p1, ActionKind::Unwatch);
	action.matchGeneration = outsider.generation; action.keepWatching = true;
	CHECK(!outsider.room.authority.Apply(outsider.room.p1, action).accepted);

	// The flag survives the wire, and an action without it decodes to false.
	Action keep = TableAction(game.room.authority, game.room.spectator, ActionKind::Unwatch);
	keep.keepWatching = true;
	Action decoded;
	nlohmann::json(keep).get_to(decoded);
	CHECK(decoded.keepWatching);
	auto legacy = nlohmann::json(keep);
	legacy.erase("keep_watching");
	legacy.get_to(decoded);
	CHECK(!decoded.keepWatching);
	legacy["keep_watching"] = 1;
	bool rejected = false;
	try { legacy.get_to(decoded); } catch (const std::exception&) { rejected = true; }
	CHECK(rejected);
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
	CHECK(failed.room.member(failed.room.spectator).spectatorLocked);
	SpectatorLockRelease release;
	ArmRelease(release, failed);
	CHECK(SpectatorBattleClosed(failed, false, true) == SessionlessClose::LeaveGame);
	CHECK(failed.room.authority.EndMatch(0, failed.generation, MatchResult::P1Win).accepted);
	failed.room.finished = failed.generation;
	CHECK(SpectatorBattleClosed(failed, true, true) == SessionlessClose::LeaveGame);
	CHECK(!failed.Unwatch(true).accepted);
	CHECK(failed.room.member(failed.room.spectator).spectatorLocked);
	CHECK(TickRelease(release, failed));
	CHECK(!release.Pending());
	CHECK(Watches(failed) && !failed.room.member(failed.room.spectator).spectatorLocked);
	CHECK(failed.room.Acknowledge(failed.room.p1) && failed.room.Acknowledge(failed.room.p2));
	CHECK(failed.room.ReadyBoth() && !failed.room.table().spectatorHold);

	// The same release with the close first and the result after it.
	LiveGame closedFirst(true);
	ArmRelease(release, closedFirst);
	CHECK(SpectatorBattleClosed(closedFirst, false, true) == SessionlessClose::LeaveGame);
	CHECK(TickRelease(release, closedFirst));
	CHECK(Watches(closedFirst) && !closedFirst.room.member(closedFirst.room.spectator).spectatorLocked);
	CHECK(closedFirst.room.authority.EndMatch(0, closedFirst.generation, MatchResult::P1Win).accepted);
	CHECK(Watches(closedFirst));

	// A spectator that merely played out or fell behind a game the players
	// finished has no failure to record: nothing is armed or sent, so its
	// lock-in stands for the next start.
	LiveGame behind(true);
	CHECK(SpectatorBattleClosed(behind, false, false) == SessionlessClose::EndView);
	CHECK(SpectatorBattleClosed(behind, true, false) == SessionlessClose::EndView);
	SpectatorLockRelease unarmed;
	CHECK(!TickRelease(unarmed, behind));
	CHECK(behind.room.member(behind.room.spectator).spectatorLocked);
	CHECK(behind.room.authority.EndMatch(0, behind.generation, MatchResult::P1Win).accepted);
	behind.room.finished = behind.generation;
	CHECK(behind.room.Acknowledge(behind.room.p1) && behind.room.Acknowledge(behind.room.p2));
	CHECK(!behind.room.ReadyBoth() && behind.room.table().spectatorHold);
}

// A spectator whose gameplay connection fails during setup has no GGPO session
// and no entered battle, so no stream failure is ever recorded. The runtime arms
// the release from the admitted generation at the failure itself. The Unwatch it
// sends for the same failure is queued once and can be lost with the replaced
// control, so it is not what clears the lock.
static void TestSetupFailureBeforeGgpoDropsTheLock() {
	using sf4e::netplay::SpectatorFailureOwesLockRelease;
	LiveGame game(true);
	CHECK(game.room.member(game.room.spectator).spectatorLocked);
	const auto& table = game.room.table();
	const bool finished = sf4e::netplay::GenerationEnded(table, game.generation);
	CHECK(!finished);
	CHECK(SpectatorFailureOwesLockRelease(true, game.generation, finished));
	SpectatorLockRelease release;
	ArmRelease(release, game);
	// The Unwatch was queued and lost with the old control: no reply, nothing applied.
	CHECK(game.room.member(game.room.spectator).spectatorLocked);
	// The result commits before the release is sent, which refuses the Unwatch.
	CHECK(game.room.authority.EndMatch(0, game.generation, MatchResult::P1Win).accepted);
	game.room.finished = game.generation;
	CHECK(!game.Unwatch(true).accepted);
	// The first release copy is lost as well; the resend reaches the recovered control.
	CHECK(!TickRelease(release, game, 1500, false));
	CHECK(release.Pending() && game.room.member(game.room.spectator).spectatorLocked);
	CHECK(!TickRelease(release, game, 1500 + SpectatorLockRelease::RetryMs - 1));
	CHECK(TickRelease(release, game, 1500 + SpectatorLockRelease::RetryMs));
	CHECK(!release.Pending());
	CHECK(Watches(game) && !game.room.member(game.room.spectator).spectatorLocked);
	CHECK(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	CHECK(game.room.ReadyBoth() && !game.room.table().spectatorHold);

	// The same failure with the result committed before the tick that records it
	// needs nothing, and neither does a fighter or a grant that named no generation.
	CHECK(!SpectatorFailureOwesLockRelease(true, game.generation, true));
	CHECK(!SpectatorFailureOwesLockRelease(false, game.generation, false));
	CHECK(!SpectatorFailureOwesLockRelease(true, 0, false));

	// A later explicit lock-in wins over the release the setup failure armed.
	LiveGame relocked(true);
	ArmRelease(release, relocked);
	CHECK(!TickRelease(release, relocked, 1500, false));
	Action press = TableAction(relocked.room.authority, relocked.room.spectator, ActionKind::LockSpectating);
	press.locked = true;
	release.Observe(press);
	CHECK(!release.Pending());
	CHECK(!TickRelease(release, relocked, 1500 + SpectatorLockRelease::RetryMs));
	CHECK(relocked.room.member(relocked.room.spectator).spectatorLocked);

	// A spectator that merely fell behind a game the room finished records no
	// failure: the rule owes nothing, the close ends the view silently, and its
	// lock-in stands.
	LiveGame behind(true);
	CHECK(behind.room.authority.EndMatch(0, behind.generation, MatchResult::P1Win).accepted);
	behind.room.finished = behind.generation;
	CHECK(!SpectatorFailureOwesLockRelease(true, behind.generation,
		sf4e::netplay::GenerationEnded(behind.room.table(), behind.generation)));
	CHECK(SpectatorBattleClosed(behind, true, false) == SessionlessClose::EndView);
	SpectatorLockRelease unarmed;
	CHECK(!TickRelease(unarmed, behind, 1500 + SpectatorLockRelease::RetryMs));
	CHECK(behind.room.member(behind.room.spectator).spectatorLocked);
}

// A lock-in the player makes after the failure is newer than the release, and
// a release still waiting to be sent must not undo it.
static void TestLaterLockInSurvivesStaleRelease() {
	// Contrast: without the cancel, the stale release clears a lock-in made after it was armed.
	LiveGame stale(false);
	SpectatorLockRelease release;
	ArmRelease(release, stale);
	CHECK(stale.room.authority.EndMatch(0, stale.generation, MatchResult::P1Win).accepted);
	CHECK(stale.room.Lock(stale.room.spectator, true).accepted);
	CHECK(TickRelease(release, stale));
	CHECK(!stale.room.member(stale.room.spectator).spectatorLocked);

	LiveGame game(false);
	ArmRelease(release, game);
	CHECK(game.room.authority.EndMatch(0, game.generation, MatchResult::P1Win).accepted);
	game.room.finished = game.generation;
	Action press = TableAction(game.room.authority, game.room.spectator, ActionKind::LockSpectating);
	press.locked = true;
	release.Observe(press);
	CHECK(game.room.authority.Apply(game.room.spectator, press).accepted);
	CHECK(!TickRelease(release, game));
	CHECK(!release.Pending());
	CHECK(game.room.member(game.room.spectator).spectatorLocked);
	CHECK(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	CHECK(!game.room.ReadyBoth() && game.room.table().spectatorHold);

	// A release that was already sent stays ordered before a later lock-in.
	LiveGame ordered(true);
	ArmRelease(release, ordered);
	CHECK(TickRelease(release, ordered));
	CHECK(!ordered.room.member(ordered.room.spectator).spectatorLocked);
	CHECK(ordered.room.authority.EndMatch(0, ordered.generation, MatchResult::P1Win).accepted);
	auto lockIn = TableAction(ordered.room.authority, ordered.room.spectator, ActionKind::LockSpectating);
	lockIn.locked = true; lockIn.actionId = NextActionId();
	CHECK(ordered.room.authority.Apply(ordered.room.spectator, lockIn).accepted);
	CHECK(ordered.room.member(ordered.room.spectator).spectatorLocked && Watches(ordered));
}

// The helper can refuse a release the runtime queued (the control was replaced
// or its queue was full). Queueing therefore does not finish it: it is sent
// again, after the match result has committed, until the authority takes it.
static void TestQueuedButUndeliveredReleaseIsResent() {
	using Step = SpectatorLockRelease::Step;
	LiveGame game(true);
	SpectatorLockRelease release;
	ArmRelease(release, game);
	CHECK(game.room.authority.EndMatch(0, game.generation, MatchResult::P1Win).accepted);
	game.room.finished = game.generation;
	// Queued, then lost with the old control: no reply, the lock is still on.
	CHECK(!TickRelease(release, game, 1500, false));
	CHECK(release.Pending() && game.room.member(game.room.spectator).spectatorLocked);
	// It waits for the authority before sending again.
	Action action;
	const auto snapshot = game.room.authority.SnapshotFor(game.room.spectator);
	CHECK(release.Next(snapshot, 1500 + SpectatorLockRelease::RetryMs - 1, &action) == Step::Await);
	// The second copy is lost too; the third reaches the recovered control.
	CHECK(!TickRelease(release, game, 1500 + SpectatorLockRelease::RetryMs, false));
	CHECK(release.Pending() && game.room.member(game.room.spectator).spectatorLocked);
	CHECK(TickRelease(release, game, 1500 + 2 * SpectatorLockRelease::RetryMs));
	CHECK(!release.Pending());
	CHECK(Watches(game) && !game.room.member(game.room.spectator).spectatorLocked);
	CHECK(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	CHECK(game.room.ReadyBoth() && !game.room.table().spectatorHold);

	// The authority took the release but its reply was lost: the projection
	// showing the member unlocked ends it, and nothing is sent again.
	LiveGame silent(true);
	ArmRelease(release, silent);
	CHECK(TickRelease(release, silent, 1500, true, false));
	CHECK(release.Pending());
	const auto unlocked = silent.room.authority.SnapshotFor(silent.room.spectator);
	CHECK(release.Next(unlocked, 1500 + SpectatorLockRelease::RetryMs, &action) == Step::Done);
	CHECK(!TickRelease(release, silent, 1500 + SpectatorLockRelease::RetryMs));

	// Undelivered until it lapses: it is given up, and the lock stays.
	LiveGame lapsed(true);
	ArmRelease(release, lapsed);
	CHECK(!TickRelease(release, lapsed, 1500, false));
	CHECK(release.Next(lapsed.room.authority.SnapshotFor(lapsed.room.spectator),
		1000 + SpectatorLockRelease::ExpiryMs, &action) == Step::Drop);
}

// A lock-in the player makes after the failure survives whatever the release
// does around it: a queued copy that was delivered but not answered, one still
// in flight, and the resends that would have followed.
static void TestLockInAfterFailureSurvivesResends() {
	LiveGame game(true);
	SpectatorLockRelease release;
	ArmRelease(release, game);
	CHECK(game.room.authority.EndMatch(0, game.generation, MatchResult::P1Win).accepted);
	game.room.finished = game.generation;
	// The release lands but its reply is lost, so it is still pending.
	CHECK(TickRelease(release, game, 1500, true, false));
	CHECK(release.Pending() && !game.room.member(game.room.spectator).spectatorLocked);
	// The player locks in again, the way the runtime dispatches the press.
	Action press = TableAction(game.room.authority, game.room.spectator, ActionKind::LockSpectating);
	press.locked = true;
	release.Observe(press);
	press.actionId = NextActionId();
	CHECK(game.room.authority.Apply(game.room.spectator, press).accepted);
	CHECK(game.room.member(game.room.spectator).spectatorLocked);
	// No resend, however long it has been.
	for (std::uint64_t now = 2500; now < 1000 + SpectatorLockRelease::ExpiryMs; now += SpectatorLockRelease::RetryMs)
		CHECK(!TickRelease(release, game, now));
	CHECK(!release.Pending() && game.room.member(game.room.spectator).spectatorLocked);
	CHECK(game.room.Acknowledge(game.room.p1) && game.room.Acknowledge(game.room.p2));
	CHECK(!game.room.ReadyBoth() && game.room.table().spectatorHold);

	// A copy still in flight when the player locks in is older than the lock-in
	// and is refused by the member's action watermark if it lands late.
	LiveGame flight(true);
	ArmRelease(release, flight);
	Action held;
	CHECK(release.Next(flight.room.authority.SnapshotFor(flight.room.spectator), 1500, &held) == SpectatorLockRelease::Step::Send);
	held.actionId = NextActionId();
	release.Queued(held.actionId, 1500);
	press = TableAction(flight.room.authority, flight.room.spectator, ActionKind::LockSpectating);
	press.locked = true;
	release.Observe(press);
	press.actionId = NextActionId();
	CHECK(flight.room.authority.Apply(flight.room.spectator, press).accepted);
	CHECK(!flight.room.authority.Apply(flight.room.spectator, held).accepted);
	CHECK(!TickRelease(release, flight, 1500 + SpectatorLockRelease::RetryMs));
	CHECK(flight.room.member(flight.room.spectator).spectatorLocked);
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

	// Checkpoints and snapshots written without these fields load with defaults.
	auto legacy = paused;
	legacy.erase("hold_age");
	legacy["snapshot"]["tables"][0].erase("spectator_hold");
	for (auto& member : legacy["snapshot"]["members"]) member.erase("spectator_locked");
	RoomAuthority old("Old");
	CHECK(old.RestoreCheckpoint(legacy));
	CHECK(!old.SnapshotView().tables[0].spectatorHold);
}

// A held table's sent snapshot says how long the hold has left, so a client
// can count it down. The room's own state and its checkpoints never carry it,
// and a snapshot from a host that does not send it reads as zero.
static void TestHoldTimeLeftIsStampedWhenSent() {
	Room room(true);
	CHECK(!room.ReadyBoth());
	CHECK(room.authority.SnapshotFor(room.p1).tables[0].holdRemainingMs == SpectatorStartHoldMs);
	CHECK(room.authority.SnapshotFor(room.spectator).tables[0].holdRemainingMs == SpectatorStartHoldMs);
	CHECK(room.authority.SnapshotView().tables[0].holdRemainingMs == 0);
	CHECK(room.authority.SnapshotFor(room.p1).tables[1].holdRemainingMs == 0);
	room.authority.AdvanceTime(5000);
	const auto sent = room.authority.SnapshotFor(room.p2);
	CHECK(sent.tables[0].holdRemainingMs == SpectatorStartHoldMs - 4000);
	const auto wire = nlohmann::json(sent);
	CHECK(wire.at("tables").at(0).at("hold_ms") == SpectatorStartHoldMs - 4000);
	CHECK(!wire.at("tables").at(1).contains("hold_ms"));
	CHECK(wire.get<Snapshot>().tables[0].holdRemainingMs == SpectatorStartHoldMs - 4000);
	CHECK(!nlohmann::json(room.authority.SnapshotView()).at("tables").at(0).contains("hold_ms"));
	CHECK(room.authority.Checkpoint().dump().find("hold_ms") == std::string::npos);
	// An older host sends none; out of range is refused.
	auto legacy = wire;
	legacy["tables"][0].erase("hold_ms");
	CHECK(legacy.get<Snapshot>().tables[0].holdRemainingMs == 0 && legacy.get<Snapshot>().tables[0].spectatorHold);
	auto wrong = wire;
	wrong["tables"][0]["hold_ms"] = SpectatorStartHoldMs + 1;
	bool threw = false;
	try { wrong.get<Snapshot>(); } catch (const std::exception&) { threw = true; }
	CHECK(threw);
	// Paused for recovery, the hold keeps its age.
	room.authority.PauseForRecovery();
	CHECK(room.authority.SnapshotFor(room.p1).tables[0].holdRemainingMs == SpectatorStartHoldMs - 4000);
	room.authority.AdvanceTime(5000);
	// Due but not yet released: still at least 1, never zero while held.
	room.authority.AdvanceTime(1000 + SpectatorStartHoldMs - 1);
	CHECK(room.authority.SnapshotFor(room.p1).tables[0].holdRemainingMs == 1);
	CHECK(StartsMatch(room.authority.AdvanceTime(1000 + SpectatorStartHoldMs)));
	CHECK(room.authority.SnapshotFor(room.p1).tables[0].holdRemainingMs == 0);
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
	if (failures) return 1;
	std::puts("RoomSpectatorLock test passed");
	return 0;
}
