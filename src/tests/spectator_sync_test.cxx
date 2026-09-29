// Pure rules behind spectating: the catch-up that keeps a spectator's backlog
// inside GGPO's frame ring, the exit of a finished match's spectator view, the
// abort report that is sent only for a game with no committed result, the
// match end that is logged once, a battle that closes after its session was
// retired, and the lock release a spectator's stream failure owes the room.
#include "../common/SpectatorCatchUp.hxx"
#include "../netplay/MatchEndRules.hxx"
#include "../session/MatchTeardownTiming.hxx"
#include <iostream>

#include "test_support.hxx"

using sf4e::SpectatorBacklogWindow;
using sf4e::SpectatorCatchUp;
namespace netplay = sf4e::netplay;
namespace room = sf4e::room;

static void CatchUp() {
	// Within the reserve nothing extra is played: arrival jitter alone never
	// speeds the view up.
	for (int backlog = -3; backlog <= SpectatorCatchUp::ReserveFrames; ++backlog)
		CHECK(SpectatorCatchUp::ExtraFrames(backlog) == 0);
	// Just past the reserve: one extra frame, never more than the excess.
	CHECK(SpectatorCatchUp::ExtraFrames(SpectatorCatchUp::ReserveFrames + 1) == 1);
	CHECK(SpectatorCatchUp::ExtraFrames(SpectatorCatchUp::ReserveFrames + 15) == 1);
	CHECK(SpectatorCatchUp::ExtraFrames(SpectatorCatchUp::ReserveFrames + 16) == 2);
	CHECK(SpectatorCatchUp::ExtraFrames(SpectatorCatchUp::ReserveFrames + 60) == 2);
	CHECK(SpectatorCatchUp::ExtraFrames(SpectatorCatchUp::ReserveFrames + 61) == SpectatorCatchUp::MaxExtraFrames);
	CHECK(SpectatorCatchUp::ExtraFrames(100000) == SpectatorCatchUp::MaxExtraFrames);
	for (int backlog = 0; backlog < 2000; ++backlog) {
		const int extra = SpectatorCatchUp::ExtraFrames(backlog);
		CHECK(extra >= 0 && extra <= SpectatorCatchUp::MaxExtraFrames && extra <= backlog);
		// Monotonic: more lag never means less catch-up.
		CHECK(extra >= SpectatorCatchUp::ExtraFrames(backlog - 1));
	}
	// A spectator a whole GGPO ring of upstream's size behind (64 frames) at
	// 60 new frames a second is back within its reserve in about a second,
	// because every update plays the stream's frame plus the extra ones.
	int backlog = 64, updates = 0;
	while (backlog > SpectatorCatchUp::ReserveFrames) {
		backlog -= SpectatorCatchUp::ExtraFrames(backlog);
		++updates;
	}
	CHECK(updates <= 60);
	std::cout << "CatchUp passed (" << updates << " updates)\n";
}

static void BacklogWindow() {
	SpectatorBacklogWindow window;
	window.Sample(0); window.CaughtUp(0);
	CHECK(window.maxBacklog == 0 && window.catchUpFrames == 0);
	window.Sample(12); window.Sample(3); window.CaughtUp(2); window.CaughtUp(1); window.CaughtUp(-4);
	CHECK(window.maxBacklog == 12 && window.catchUpFrames == 3);
	std::cout << "BacklogWindow passed\n";
}

static void MatchEndLogOnce() {
	// The same committed end can arrive more than once; only the first is logged.
	netplay::MatchEndLog log;
	CHECK(log.First(0, 2));
	CHECK(!log.First(0, 2));
	CHECK(!log.First(0, 2));
	CHECK(log.First(0, 3));
	CHECK(log.First(1, 3));
	CHECK(!log.First(1, 3));
	// Generation 0 is logged too, but only once.
	netplay::MatchEndLog fresh;
	CHECK(fresh.First(0, 0));
	CHECK(!fresh.First(0, 0));
	// Two tables can interleave their copies: each remembers its own last end.
	netplay::MatchEndLog mixed;
	CHECK(mixed.First(0, 2));
	CHECK(mixed.First(1, 5));
	CHECK(!mixed.First(0, 2));
	CHECK(!mixed.First(1, 5));
	CHECK(mixed.First(0, 3));
	CHECK(!mixed.First(0, 3));
	CHECK(mixed.First(1, 6));
	std::cout << "MatchEndLogOnce passed\n";
}

static void StaleAbortNotSent() {
	room::Table table;
	table.matchGeneration = 2;
	table.phase = room::TablePhase::Playing;
	CHECK(!netplay::GenerationEnded(table, 2));
	table.phase = room::TablePhase::Paused;
	CHECK(!netplay::GenerationEnded(table, 2));
	// A result committed 15 s earlier and the table back at Waiting: the game
	// is over, and an Unwatch for it would be rejected with WrongGeneration.
	for (const auto phase : {room::TablePhase::Idle, room::TablePhase::Waiting, room::TablePhase::Ready, room::TablePhase::Closed}) {
		table.phase = phase;
		CHECK(netplay::GenerationEnded(table, 2));
	}
	// The table already started a newer game.
	table.phase = room::TablePhase::Playing;
	table.matchGeneration = 3;
	CHECK(netplay::GenerationEnded(table, 2));
	// The native grant for generation 3 arrived before the room snapshot
	// did, and setup failed in between: the projection still shows the
	// previous game, ended or not. That is no evidence that 3 ended, so the
	// abort is sent (or kept for retry) rather than dropped.
	table.matchGeneration = 2;
	for (const auto phase : {room::TablePhase::Playing, room::TablePhase::Waiting, room::TablePhase::Ready}) {
		table.phase = phase;
		CHECK(!netplay::GenerationEnded(table, 3));
	}
	table.matchGeneration = 0; table.phase = room::TablePhase::Idle;
	CHECK(!netplay::GenerationEnded(table, 1));
	std::cout << "StaleAbortNotSent passed\n";
}

static void SpectatorExit() {
	using Timing = sf4e::session::MatchTeardownTiming;
	using Exit = Timing::SpectatorExit;
	// P1 still streaming: keep watching until the bound.
	CHECK(Timing::SpectatorExitStep(false, false, false) == Exit::Wait);
	CHECK(Timing::SpectatorExitStep(false, true, false) == Exit::Wait);
	// P1 left: play out what reached GGPO, then retire quietly without
	// waiting for the timeout.
	CHECK(Timing::SpectatorExitStep(true, false, false) == Exit::Wait);
	CHECK(Timing::SpectatorExitStep(true, true, false) == Exit::Retire);
	CHECK(Timing::SpectatorExitStep(true, true, true) == Exit::Retire);
	// At the bound with P1 gone but frames left (a gated battle): still
	// quiet, since nothing more could have arrived anyway.
	CHECK(Timing::SpectatorExitStep(true, false, true) == Exit::Retire);
	// At the bound while P1 is still streaming the view is cut short: the
	// only case that keeps the timeout notice.
	CHECK(Timing::SpectatorExitStep(false, false, true) == Exit::RetireCutShort);
	CHECK(Timing::SpectatorExitStep(false, true, true) == Exit::RetireCutShort);
	// The deadline is armed once.
	Timing timing;
	timing.ArmSpectatorExit(1000);
	timing.ArmSpectatorExit(5000);
	CHECK(!timing.SpectatorExitTimedOut(1000 + Timing::SpectatorExitTimeoutMs - 1));
	CHECK(timing.SpectatorExitTimedOut(1000 + Timing::SpectatorExitTimeoutMs));
	std::cout << "SpectatorExit passed\n";
}

static void SessionlessCloseRules() {
	using netplay::SessionlessClose;
	using netplay::SessionlessCloseAction;
	// (entered, entered generation, closed generation, session generation,
	//  recovering, match in progress, spectator, end committed, spectator stream failed)
	// A spectator whose session played out or that fell behind a finished game:
	// end the view, report nothing, and the controller leaves Playing.
	CHECK(SessionlessCloseAction(true, 4, 4, 4, false, true, true, false, false) == SessionlessClose::EndView);
	CHECK(SessionlessCloseAction(true, 4, 4, 4, false, true, true, true, false) == SessionlessClose::EndView);
	// A start that failed before the battle began (Preparing) is the same.
	CHECK(SessionlessCloseAction(true, 4, 4, 4, false, true, true, false, false) == SessionlessClose::EndView);
	// A spectator whose own GGPO stream failed leaves this game, and a result
	// committed after the failure does not turn that into a plain end of view.
	CHECK(SessionlessCloseAction(true, 4, 4, 4, false, true, true, false, true) == SessionlessClose::LeaveGame);
	CHECK(SessionlessCloseAction(true, 4, 4, 4, false, true, true, true, true) == SessionlessClose::LeaveGame);
	// A fighter whose GGPO failed with no committed result reports an abort; with
	// the end committed there is nothing to report. The spectator flag is moot.
	CHECK(SessionlessCloseAction(true, 4, 4, 4, false, true, false, false, false) == SessionlessClose::Abort);
	CHECK(SessionlessCloseAction(true, 4, 4, 4, false, true, false, false, true) == SessionlessClose::Abort);
	CHECK(SessionlessCloseAction(true, 4, 4, 4, false, true, false, true, false) == SessionlessClose::EndView);
	// Something else already ends the game, or the close is for another entry.
	CHECK(SessionlessCloseAction(true, 4, 4, 4, true, true, true, false, true) == SessionlessClose::Ignore);
	CHECK(SessionlessCloseAction(false, 0, 4, 4, false, true, true, false, true) == SessionlessClose::Ignore);
	CHECK(SessionlessCloseAction(true, 4, 3, 4, false, true, true, false, true) == SessionlessClose::Ignore);
	CHECK(SessionlessCloseAction(true, 4, 4, 5, false, true, true, false, true) == SessionlessClose::Ignore);
	CHECK(SessionlessCloseAction(true, 4, 0, 4, false, true, true, false, true) == SessionlessClose::Ignore);
	CHECK(SessionlessCloseAction(true, 4, 4, 4, false, false, true, false, true) == SessionlessClose::Ignore);
	std::cout << "SessionlessCloseRules passed\n";
}

// A snapshot for member 7 at table 1 with generation 4 playing, the spectator
// watching by choice.
static room::Snapshot ReleaseSnapshot() {
	room::Snapshot snapshot;
	snapshot.roomEpoch = 9; snapshot.revision = 20; snapshot.localMember = 7;
	room::Member member; member.id = 7; member.table = 1; member.seat = -1; member.spectatorLocked = true;
	snapshot.members.push_back(member);
	auto& table = snapshot.tables[1];
	table.id = 1; table.revision = 33; table.matchGeneration = 4; table.phase = room::TablePhase::Playing;
	table.spectators.push_back(7);
	return snapshot;
}

static void SpectatorLockReleaseLifecycle() {
	using Release = netplay::SpectatorLockRelease;
	room::Action action;
	Release release;
	auto snapshot = ReleaseSnapshot();
	// Nothing armed, nothing sent.
	CHECK(release.Next(snapshot, 1000, &action) == Release::Step::Idle);

	// Armed for generation 4: an unlock is sent, whatever phase the table is in
	// (a result committed since must not cancel it), stamped with the current
	// revisions and the failed generation.
	release.Arm(1, 4, 9, 1000);
	CHECK(release.Next(snapshot, 1500, &action) == Release::Step::Send);
	CHECK(action.kind == room::ActionKind::LockSpectating && !action.locked && action.table == 1 &&
		action.roomEpoch == 9 && action.revision == 20 && action.tableRevision == 33 && action.matchGeneration == 4);
	snapshot.tables[1].phase = room::TablePhase::Waiting; snapshot.tables[1].revision = 40;
	CHECK(release.Next(snapshot, 1600, &action) == Release::Step::Send && action.tableRevision == 40);
	// A newer generation started meanwhile: the lock was never dropped, so it still is.
	snapshot.tables[1].matchGeneration = 5; snapshot.tables[1].phase = room::TablePhase::Playing;
	CHECK(release.Next(snapshot, 1700, &action) == Release::Step::Send);
	// A projection that has not reached the failed generation waits.
	snapshot.tables[1].matchGeneration = 3;
	CHECK(release.Next(snapshot, 1700, &action) == Release::Step::Wait);
	snapshot.tables[1].matchGeneration = 4;
	CHECK(release.Next(snapshot, 1700, &action) == Release::Step::Send);

	// Queued is not delivered: the release stays pending, waits out RetryMs for
	// the authority, then goes again.
	release.Queued(50, 1800);
	CHECK(release.Pending() && release.Next(snapshot, 1800, &action) == Release::Step::Await);
	CHECK(release.Next(snapshot, 1800 + Release::RetryMs - 1, &action) == Release::Step::Await);
	CHECK(release.Next(snapshot, 1800 + Release::RetryMs, &action) == Release::Step::Send);
	// A reply for another action does not end it; the acceptance of the latest copy does.
	release.Acknowledged(49);
	CHECK(release.Pending());
	release.Queued(51, 2800);
	release.Acknowledged(50);
	CHECK(release.Pending());
	release.Acknowledged(51);
	CHECK(!release.Pending() && release.Next(snapshot, 3000, &action) == Release::Step::Idle);

	// A projection that shows the member unlocked confirms a release whose reply was lost.
	auto unlocked = snapshot; unlocked.members[0].spectatorLocked = false;
	release.Arm(1, 4, 9, 1000);
	release.Queued(52, 1100);
	CHECK(release.Next(unlocked, 1200, &action) == Release::Step::Done);
	// The same before anything was queued: nothing is left to release.
	release.Arm(1, 4, 9, 1000);
	CHECK(release.Next(unlocked, 1200, &action) == Release::Step::Done);
	// Arming again starts over and drops the earlier copy.
	release.Queued(53, 1100);
	release.Arm(1, 4, 9, 1000);
	CHECK(release.Next(snapshot, 1200, &action) == Release::Step::Send);
	release.Acknowledged(53);
	CHECK(release.Pending());
	// Queued and unconfirmed still lapses with the clock, and a lock-in cancels it.
	release.Queued(54, 1100);
	CHECK(release.Next(snapshot, 1000 + Release::ExpiryMs, &action) == Release::Step::Drop);
	room::Action lockIn; lockIn.kind = room::ActionKind::LockSpectating; lockIn.locked = true;
	release.Observe(lockIn);
	CHECK(!release.Pending());

	// An explicit lock-in after the failure stands; an unlock or another kind
	// of action does not cancel it.
	release.Arm(1, 4, 9, 1000);
	room::Action press; press.kind = room::ActionKind::LockSpectating; press.locked = false;
	release.Observe(press);
	press.kind = room::ActionKind::Watch; press.locked = true;
	release.Observe(press);
	CHECK(release.Pending());
	press.kind = room::ActionKind::LockSpectating;
	release.Observe(press);
	CHECK(!release.Pending() && release.Next(snapshot, 1100, &action) == Release::Step::Idle);

	// It lapses with the room, the table, the role and the clock.
	release.Arm(1, 4, 8, 1000);
	CHECK(release.Next(snapshot, 1100, &action) == Release::Step::Drop);
	release.Arm(1, 4, 9, 1000);
	auto moved = snapshot; moved.members[0].table = 0;
	CHECK(release.Next(moved, 1100, &action) == Release::Step::Drop);
	auto seated = snapshot; seated.members[0].seat = 0;
	CHECK(release.Next(seated, 1100, &action) == Release::Step::Drop);
	auto gone = snapshot; gone.tables[1].spectators.clear();
	CHECK(release.Next(gone, 1100, &action) == Release::Step::Drop);
	auto absent = snapshot; absent.localMember = 8;
	CHECK(release.Next(absent, 1100, &action) == Release::Step::Drop);
	CHECK(release.Next(snapshot, 1000 + Release::ExpiryMs - 1, &action) == Release::Step::Send);
	CHECK(release.Next(snapshot, 1000 + Release::ExpiryMs, &action) == Release::Step::Drop);
	std::cout << "SpectatorLockRelease passed\n";
}

static void SetupFailureOwesLockRelease() {
	// A spectator's local failure of an admitted generation owes its lock release,
	// with no battle or GGPO session needed to have been reached.
	CHECK(netplay::SpectatorFailureOwesLockRelease(true, 4, false));
	// A finished game, a fighter, or no admitted generation owe nothing.
	CHECK(!netplay::SpectatorFailureOwesLockRelease(true, 4, true));
	CHECK(!netplay::SpectatorFailureOwesLockRelease(false, 4, false));
	CHECK(!netplay::SpectatorFailureOwesLockRelease(true, 0, false));
	// A spectator that only played out the game is silent at the close.
	CHECK(netplay::SessionlessCloseAction(true, 4, 4, 4, false, true, true, false, false) == netplay::SessionlessClose::EndView);
	std::cout << "SetupFailureOwesLockRelease passed\n";
}

static void StaleEntryIsReleased() {
	// A newer grant while the entry of an older generation is still held and its
	// battle is gone: release the old one so the new grant is entered.
	CHECK(netplay::StaleMatchEntry(true, true, 4, 5, false));
	// The same generation, no live session, GGPO still running, or nothing entered.
	CHECK(!netplay::StaleMatchEntry(true, true, 5, 5, false));
	CHECK(!netplay::StaleMatchEntry(false, true, 4, 5, false));
	CHECK(!netplay::StaleMatchEntry(true, true, 4, 5, true));
	CHECK(!netplay::StaleMatchEntry(true, false, 0, 5, false));

	std::cout << "StaleEntryIsReleased passed\n";
}

int main() {
	CatchUp();
	BacklogWindow();
	MatchEndLogOnce();
	StaleAbortNotSent();
	SpectatorExit();
	SessionlessCloseRules();
	SpectatorLockReleaseLifecycle();
	SetupFailureOwesLockRelease();
	StaleEntryIsReleased();
	std::cout << "Spectator sync tests passed\n";
	return 0;
}
