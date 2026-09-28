// Pure rules behind the v0.9.9 spectator field reports: the catch-up that keeps
// a spectator inside GGPO's frame ring, the exit of a finished match's
// spectator view, the stale abort that is no longer sent, and the match end
// that was logged three times.
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
	// sf4e.1.log 23:04:35: the same committed end arrived three times.
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
	std::cout << "MatchEndLogOnce passed\n";
}

static void StaleAbortNotSent() {
	room::Table table;
	table.matchGeneration = 2;
	table.phase = room::TablePhase::Playing;
	CHECK(!netplay::GenerationEnded(table, 2));
	table.phase = room::TablePhase::Paused;
	CHECK(!netplay::GenerationEnded(table, 2));
	// sf4e.1.log 23:04:50: the result was committed 15 s earlier and the table
	// was back to Waiting; the Unwatch was rejected with WrongGeneration.
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
	// P1 left (sf4e.1.log 23:04:36): play out what reached GGPO, then retire
	// quietly, instead of the 15 s timeout and error the log shows.
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
	// The deadline itself is unchanged and armed once.
	Timing timing;
	timing.ArmSpectatorExit(1000);
	timing.ArmSpectatorExit(5000);
	CHECK(!timing.SpectatorExitTimedOut(1000 + Timing::SpectatorExitTimeoutMs - 1));
	CHECK(timing.SpectatorExitTimedOut(1000 + Timing::SpectatorExitTimeoutMs));
	std::cout << "SpectatorExit passed\n";
}

int main() {
	CatchUp();
	BacklogWindow();
	MatchEndLogOnce();
	StaleAbortNotSent();
	SpectatorExit();
	std::cout << "Spectator sync tests passed\n";
	return 0;
}
