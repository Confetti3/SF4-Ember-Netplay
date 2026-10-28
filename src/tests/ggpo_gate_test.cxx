// Pure unit tests for the GGPO simulation gate model and error classifier.

#include <stdio.h>

#include "../common/sf4e__GgpoGate.hxx"
#include "../common/sf4e__GgpoAbortLatch.hxx"

#include <string.h>
#include "test_support.hxx"

using namespace sf4e::gate;

static GgpoGateModel Fresh() {
	GgpoGateModel g;
	g.Reset();
	return g;
}

static void TestLifecycleGating() {
	GgpoGateModel g = Fresh();
	EXPECT(!g.CanAdvanceDeterministicFrame()); // no session

	g.OnSessionStarted();
	EXPECT(!g.CanAdvanceDeterministicFrame()); // startup remains gated until RUNNING

	g.OnRunning();
	EXPECT(g.CanAdvanceDeterministicFrame());

	g.OnBattleClosing();
	EXPECT(!g.CanAdvanceDeterministicFrame());

	g.OnSessionClosed();
	EXPECT(g.phase == PHASE_NO_SESSION);
	EXPECT(!g.CanAdvanceDeterministicFrame());
}

static void TestConnectionWarningDoesNotBlock() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();

	EXPECT(g.OnConnectionInterrupted(1000));  // newly active → alert once
	EXPECT(!g.OnConnectionInterrupted(1500)); // duplicate → no second alert
	EXPECT(g.connectionWarningActive);
	EXPECT(g.connectionWarningStartedAtMs == 1000);

	// The warning must not stop simulation.
	EXPECT(g.CanAdvanceDeterministicFrame());

	EXPECT(g.OnConnectionResumed());
	EXPECT(!g.OnConnectionResumed()); // no warning active → no alert
	EXPECT(g.CanAdvanceDeterministicFrame());
}

static void TestWarningPlusManualPause() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();

	g.OnConnectionInterrupted(100);
	g.SetManualPause(true);
	EXPECT(!g.CanAdvanceDeterministicFrame());

	// Resume clears only the warning; the manual pause must survive.
	g.OnConnectionResumed();
	EXPECT(g.manualPause);
	EXPECT(!g.CanAdvanceDeterministicFrame());

	g.SetManualPause(false);
	EXPECT(g.CanAdvanceDeterministicFrame());
}

static void TestThresholdPlusResume() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();

	g.OnConnectionInterrupted(100);
	g.OnPredictionThreshold();
	EXPECT(g.predictionStalled);

	// A resume event clears the warning but NOT the prediction stall; the
	// stall ends only when GGPO accepts progression again.
	g.OnConnectionResumed();
	EXPECT(g.predictionStalled);
	EXPECT(!g.connectionWarningActive);

	g.OnFrameAccepted();
	EXPECT(!g.predictionStalled);

	// The prediction stall never latches the central gate: GGPO enforces it
	// by refusing input, so the gate stays open.
	g.OnPredictionThreshold();
	EXPECT(g.CanAdvanceDeterministicFrame());
}

static void TestFatalImmuneToResume() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();

	g.OnConnectionInterrupted(100);
	g.OnFatal();
	EXPECT(!g.CanAdvanceDeterministicFrame());

	g.OnConnectionResumed();
	EXPECT(g.fatalError);
	EXPECT(!g.CanAdvanceDeterministicFrame());

	// RUNNING (stale event) cannot revive a fatal session either.
	g.OnRunning();
	EXPECT(!g.CanAdvanceDeterministicFrame());
}

static void TestNewSessionClearsPerSessionStateButKeepsPause() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();
	g.OnFatal();
	g.OnConnectionInterrupted(100);
	g.OnPredictionThreshold();
	g.SetManualPause(true);

	g.OnSessionStarted(); // rematch
	EXPECT(!g.fatalError);
	EXPECT(!g.connectionWarningActive);
	EXPECT(!g.predictionStalled);
	EXPECT(g.manualPause); // user intent survives the rematch
	g.OnRunning();
	EXPECT(!g.CanAdvanceDeterministicFrame()); // still paused

	g.SetManualPause(false);
	EXPECT(g.CanAdvanceDeterministicFrame());
}

static void TestRunningOnlyFromWaiting() {
	GgpoGateModel g = Fresh();
	// A stray RUNNING with no session must not open the gate.
	g.OnRunning();
	EXPECT(g.phase == PHASE_NO_SESSION);
	EXPECT(!g.CanAdvanceDeterministicFrame());

	// A stray RUNNING while closing must not reopen the battle.
	g.OnSessionStarted();
	g.OnRunning();
	g.OnBattleClosing();
	g.OnRunning();
	EXPECT(g.phase == PHASE_BATTLE_CLOSING);
}

// --- who owns the native battle (F-016) ---

// fSystem::simGate is aggregate-initialised with the phase alone, so every
// other field is zero: that must read as an offline battle.
static void TestAggregateInitialisationIsOffline() {
	GgpoGateModel g = { PHASE_NO_SESSION };
	EXPECT(g.nativeBattle == NATIVE_BATTLE_OFFLINE);
	EXPECT(!g.NativeExitRequired());
	EXPECT(g.LocalControllerOwnsBattle(false));
	EXPECT(g.MayAdvance(false, true));
}

static void TestOfflineBattleIsNeverOrphaned() {
	GgpoGateModel g = Fresh();
	g.OnSessionClosed(); // a shutdown at the menu, no battle claimed
	EXPECT(!g.NativeExitRequired());
	EXPECT(g.LocalControllerOwnsBattle(false));
	EXPECT(!g.OnNativeBattleClosed()); // nothing for the runtime to hear about
	EXPECT(!g.NativeExitRequired());
}

static void TestLosingTheSessionOrphansTheBattle() {
	GgpoGateModel g = Fresh();
	g.OnNetplayBattleClaimed();
	g.OnSessionStarted();
	g.OnRunning();
	EXPECT(!g.LocalControllerOwnsBattle(true)); // GGPO owns the pad while live
	EXPECT(!g.NativeExitRequired());

	g.OnSessionClosed(); // room closed, helper died, leave mid-fight
	EXPECT(g.NativeExitRequired());
	EXPECT(!g.LocalControllerOwnsBattle(false)); // never the local pad on both slots
	EXPECT(g.MayAdvance(false, true));            // the engine must run to leave
	EXPECT(g.OnOrphanFrame());                    // reported once
	EXPECT(!g.OnOrphanFrame());

	EXPECT(g.OnNativeBattleClosed()); // CloseBattle: an orphan, but a netplay battle
	EXPECT(!g.NativeExitRequired());
	EXPECT(g.LocalControllerOwnsBattle(false));
}

// CloseBattle reports whether the battle it closes was a netplay battle, live
// session or not, once: the runtime learns of every one, and nothing twice.
static void TestClosingReportsANetplayBattleOnce() {
	GgpoGateModel live = Fresh();
	live.OnNetplayBattleClaimed();
	live.OnSessionStarted();
	live.OnRunning();
	EXPECT(live.OnNativeBattleClosed());
	EXPECT(!live.OnNativeBattleClosed());
	// A session retired after the close (the spectator drain) orphans nothing.
	live.OnSessionClosed();
	EXPECT(!live.NativeExitRequired());
	EXPECT(!live.OnNativeBattleClosed());

	// A spectator whose stream ended: the session went first, the battle after.
	GgpoGateModel spectator = Fresh();
	spectator.OnNetplayBattleClaimed();
	spectator.OnSessionStarted();
	spectator.OnSessionClosed();
	EXPECT(spectator.NativeExitRequired());
	EXPECT(spectator.OnNativeBattleClosed());
}

// A failure before the session starts (no endpoints, no controller, a
// refused start) aborts with no session. The claim alone must orphan.
static void TestClaimWithoutSessionOrphansOnClose() {
	GgpoGateModel g = Fresh();
	g.OnNetplayBattleClaimed();
	g.OnFatal();
	g.OnSessionClosed();
	EXPECT(g.NativeExitRequired());
	// An abort never gates the battle it has to leave, nor later offline play.
	EXPECT(g.MayAdvance(false, true));
	EXPECT(g.OnNativeBattleClosed());
	EXPECT(g.MayAdvance(false, true));
	EXPECT(!g.MayAdvance(false, false)); // only the manual gate remains
}

// StartGGPO retires a leftover session before starting the next one. The
// new session re-claims the battle in the same call.
static void TestLeftoverRetireThenStartReclaims() {
	GgpoGateModel g = Fresh();
	g.OnNetplayBattleClaimed();
	g.OnSessionClosed(); // leftover_before_start
	EXPECT(g.NativeExitRequired());
	g.OnSessionStarted();
	EXPECT(!g.NativeExitRequired());
	EXPECT(g.nativeBattle == NATIVE_BATTLE_NETPLAY);
	EXPECT(g.orphanFrames == 0);
}

// CloseBattle runs before the session is retired, and the spectator drain
// retires it later still. Neither may orphan the battle that just ended.
static void TestBattleClosedBeforeRetireIsNotAnOrphan() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();
	g.OnBattleClosing();
	g.OnNativeBattleClosed();
	g.OnSessionClosed();
	EXPECT(!g.NativeExitRequired());
	EXPECT(g.LocalControllerOwnsBattle(false));
}

static void TestMayAdvanceFollowsTheSessionWhileLive() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	EXPECT(!g.MayAdvance(true, true)); // waiting for RUNNING
	g.OnRunning();
	EXPECT(g.MayAdvance(true, true));
	EXPECT(!g.MayAdvance(true, false)); // manual pause
	g.OnFatal();
	EXPECT(!g.MayAdvance(true, true));
	g.OnSessionClosed();
	EXPECT(g.MayAdvance(false, true)); // the fatal state dies with the session
}

static void TestOrphanOverdueFiresOnce() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnSessionClosed();
	int overdue = 0;
	for (uint32_t frame = 0; frame < 10; frame++) {
		g.OnOrphanFrame();
		if (g.OrphanOverdue(5)) overdue++;
	}
	EXPECT(overdue == 1);
	EXPECT(g.orphanFrames == 10);
}

static void TestClassifier() {
	EXPECT(ClassifyGgpoResult(0) == POLICY_CONTINUE);
	EXPECT(ClassifyGgpoResult(4) == POLICY_STALL_PREDICTION);
	EXPECT(ClassifyGgpoResult(6) == POLICY_SKIP_NOT_SYNCED);
	EXPECT(ClassifyGgpoResult(7) == POLICY_SKIP_OTHER);
	EXPECT(ClassifyGgpoResult(8) == POLICY_SKIP_OTHER);
	EXPECT(ClassifyGgpoResult(-1) == POLICY_FATAL);
	EXPECT(ClassifyGgpoResult(1) == POLICY_FATAL);
	EXPECT(ClassifyGgpoResult(2) == POLICY_FATAL);
	EXPECT(ClassifyGgpoResult(3) == POLICY_FATAL);
	EXPECT(ClassifyGgpoResult(5) == POLICY_FATAL);
	EXPECT(ClassifyGgpoResult(9) == POLICY_FATAL);
	EXPECT(ClassifyGgpoResult(10) == POLICY_FATAL);
	EXPECT(ClassifyGgpoResult(11) == POLICY_FATAL);
	EXPECT(ClassifyGgpoResult(12345) == POLICY_FATAL); // unknown codes are fatal
}

// The session must never be closed from inside a GGPO callback: the fork
// keeps calling the advance-frame callback after it returns. Outside a
// callback an abort proceeds at once; inside one it is latched and handed
// out only once every callback frame has unwound.
static void TestAbortLatchOutsideCallbackIsImmediate() {
	AbortLatch latch;
	EXPECT(!latch.InCallback());
	EXPECT(!latch.Request("sync failed")); // not latched: caller closes now
	EXPECT(!latch.pending);
	char reason[256] = "untouched";
	EXPECT(!latch.Take(reason, sizeof(reason)));
	EXPECT(strcmp(reason, "untouched") == 0);
}

static void TestAbortLatchInsideCallbackDefers() {
	AbortLatch latch;
	char reason[256] = {};
	{
		AbortLatch::Scope callback(latch);
		EXPECT(latch.InCallback());
		EXPECT(latch.Request("first reason"));
		EXPECT(latch.pending);
		// A second abort in the same burst keeps the first explanation.
		EXPECT(latch.Request("second reason"));
		EXPECT(strcmp(latch.reason, "first reason") == 0);
		// Still inside the callback: nothing may be taken yet.
		EXPECT(!latch.Take(reason, sizeof(reason)));
		EXPECT(reason[0] == '\0');
	}
	EXPECT(!latch.InCallback());
	EXPECT(latch.Take(reason, sizeof(reason)));
	EXPECT(strcmp(reason, "first reason") == 0);
	EXPECT(!latch.pending);
	EXPECT(latch.reason[0] == '\0');
	EXPECT(!latch.Take(reason, sizeof(reason))); // drained exactly once
}

static void TestAbortLatchNestedCallbacks() {
	// ggpo_advance_frame inside the rollback callback runs the save
	// callback: depth two. The abort is taken only after both unwind.
	AbortLatch latch;
	char reason[256] = {};
	{
		AbortLatch::Scope outer(latch);
		{
			AbortLatch::Scope inner(latch);
			EXPECT(latch.depth == 2);
			EXPECT(latch.Request("buffer full"));
		}
		EXPECT(latch.depth == 1);
		EXPECT(!latch.Take(reason, sizeof(reason)));
	}
	EXPECT(latch.depth == 0);
	EXPECT(latch.Take(reason, sizeof(reason)));
	EXPECT(strcmp(reason, "buffer full") == 0);
}

static void TestAbortLatchResetDropsPending() {
	AbortLatch latch;
	{
		AbortLatch::Scope callback(latch);
		EXPECT(latch.Request("stale"));
	}
	latch.Reset(); // a new session started before the outer tick drained it
	char reason[256] = {};
	EXPECT(!latch.Take(reason, sizeof(reason)));
	EXPECT(!latch.pending);
}

// Model one native round transition with a save before it. Native idle still
// advances the engine, as it does in BattleUpdate's old bypass branch. Replaying
// the resulting GGPO frame count must land on the same engine state. The old
// (session && !idle) routing loses three transition frames in this fixture.
static void TestRoundTransitionUsesOneTimeline() {
	GgpoGateModel gate{};
	gate.OnSessionStarted();
	EXPECT(!gate.UsesGgpoTimeline(true, true)); // preserve initial bootstrap
	gate.OnRunning();
	EXPECT(!gate.UsesGgpoTimeline(true, true));
	EXPECT(gate.UsesGgpoTimeline(true, false));
	int engine = 0, ggpoFrames = 0;
	const auto idleAt = [](int frame) { return frame >= 2 && frame < 5; };
	for (int tick = 0; tick < 8; ++tick) {
		if (gate.UsesGgpoTimeline(true, idleAt(engine))) ++ggpoFrames;
		++engine;
	}
	int replayEngine = 0;
	for (int frame = 0; frame < ggpoFrames; ++frame) ++replayEngine;
	EXPECT(ggpoFrames == 8);
	EXPECT(replayEngine == engine);
	// A rollback into idle keeps the latch; a rematch gets a new bootstrap.
	EXPECT(gate.UsesGgpoTimeline(true, true));
	gate.OnSessionClosed();
	EXPECT(!gate.UsesGgpoTimeline(false, false));
	gate.OnSessionStarted();
	EXPECT(!gate.UsesGgpoTimeline(true, true));
	gate.Reset();
	EXPECT(!gate.timelineStarted);
}

int main() {
	TestRoundTransitionUsesOneTimeline();
	TestAbortLatchOutsideCallbackIsImmediate();
	TestAbortLatchInsideCallbackDefers();
	TestAbortLatchNestedCallbacks();
	TestAbortLatchResetDropsPending();
	TestLifecycleGating();
	TestConnectionWarningDoesNotBlock();
	TestWarningPlusManualPause();
	TestThresholdPlusResume();
	TestFatalImmuneToResume();
	TestNewSessionClearsPerSessionStateButKeepsPause();
	TestRunningOnlyFromWaiting();
	TestAggregateInitialisationIsOffline();
	TestOfflineBattleIsNeverOrphaned();
	TestLosingTheSessionOrphansTheBattle();
	TestClaimWithoutSessionOrphansOnClose();
	TestLeftoverRetireThenStartReclaims();
	TestBattleClosedBeforeRetireIsNotAnOrphan();
	TestClosingReportsANetplayBattleOnce();
	TestMayAdvanceFollowsTheSessionWhileLive();
	TestOrphanOverdueFiresOnce();
	TestClassifier();

	if (TEST_FAILURES()) {
		printf("%d failure(s)\n", TEST_FAILURES());
		return 1;
	}
	printf("ggpo_gate_test: all tests passed\n");
	return 0;
}
