// Pure unit tests for the GGPO simulation gate model and error classifier.

#include <stdio.h>

#include "../common/sf4e__GgpoGate.hxx"
#include "../common/sf4e__GgpoAbortLatch.hxx"

#include <string.h>

using namespace sf4e::gate;

static int g_failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
			g_failures++;                                                    \
		}                                                                    \
	} while (0)

static GgpoGateModel Fresh() {
	GgpoGateModel g;
	g.Reset();
	return g;
}

static void TestLifecycleGating() {
	GgpoGateModel g = Fresh();
	CHECK(!g.CanAdvanceDeterministicFrame()); // no session

	g.OnSessionStarted();
	CHECK(!g.CanAdvanceDeterministicFrame()); // startup remains gated until RUNNING

	g.OnRunning();
	CHECK(g.CanAdvanceDeterministicFrame());

	g.OnBattleClosing();
	CHECK(!g.CanAdvanceDeterministicFrame());

	g.OnSessionClosed();
	CHECK(g.phase == PHASE_NO_SESSION);
	CHECK(!g.CanAdvanceDeterministicFrame());
}

static void TestConnectionWarningDoesNotBlock() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();

	CHECK(g.OnConnectionInterrupted(1000));  // newly active → alert once
	CHECK(!g.OnConnectionInterrupted(1500)); // duplicate → no second alert
	CHECK(g.connectionWarningActive);
	CHECK(g.connectionWarningStartedAtMs == 1000);

	// The warning must not stop simulation.
	CHECK(g.CanAdvanceDeterministicFrame());

	CHECK(g.OnConnectionResumed());
	CHECK(!g.OnConnectionResumed()); // no warning active → no alert
	CHECK(g.CanAdvanceDeterministicFrame());
}

static void TestWarningPlusManualPause() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();

	g.OnConnectionInterrupted(100);
	g.SetManualPause(true);
	CHECK(!g.CanAdvanceDeterministicFrame());

	// Resume clears only the warning; the manual pause must survive.
	g.OnConnectionResumed();
	CHECK(g.manualPause);
	CHECK(!g.CanAdvanceDeterministicFrame());

	g.SetManualPause(false);
	CHECK(g.CanAdvanceDeterministicFrame());
}

static void TestThresholdPlusResume() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();

	g.OnConnectionInterrupted(100);
	g.OnPredictionThreshold();
	CHECK(g.predictionStalled);

	// A resume event clears the warning but NOT the prediction stall — the
	// stall ends only when GGPO accepts progression again.
	g.OnConnectionResumed();
	CHECK(g.predictionStalled);
	CHECK(!g.connectionWarningActive);

	g.OnFrameAccepted();
	CHECK(!g.predictionStalled);

	// The prediction stall never latches the central gate: GGPO enforces it
	// by refusing input, so the gate stays open.
	g.OnPredictionThreshold();
	CHECK(g.CanAdvanceDeterministicFrame());
}

static void TestFatalImmuneToResume() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	g.OnRunning();

	g.OnConnectionInterrupted(100);
	g.OnFatal();
	CHECK(!g.CanAdvanceDeterministicFrame());

	g.OnConnectionResumed();
	CHECK(g.fatalError);
	CHECK(!g.CanAdvanceDeterministicFrame());

	// RUNNING (stale event) cannot revive a fatal session either.
	g.OnRunning();
	CHECK(!g.CanAdvanceDeterministicFrame());
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
	CHECK(!g.fatalError);
	CHECK(!g.connectionWarningActive);
	CHECK(!g.predictionStalled);
	CHECK(g.manualPause); // user intent survives the rematch
	g.OnRunning();
	CHECK(!g.CanAdvanceDeterministicFrame()); // still paused

	g.SetManualPause(false);
	CHECK(g.CanAdvanceDeterministicFrame());
}

static void TestRunningOnlyFromWaiting() {
	GgpoGateModel g = Fresh();
	// A stray RUNNING with no session must not open the gate.
	g.OnRunning();
	CHECK(g.phase == PHASE_NO_SESSION);
	CHECK(!g.CanAdvanceDeterministicFrame());

	// A stray RUNNING while closing must not reopen the battle.
	g.OnSessionStarted();
	g.OnRunning();
	g.OnBattleClosing();
	g.OnRunning();
	CHECK(g.phase == PHASE_BATTLE_CLOSING);
}

// --- who owns the native battle (F-016) ---

// fSystem::simGate is aggregate-initialised with the phase alone, so every
// other field is zero: that must read as an offline battle.
static void TestAggregateInitialisationIsOffline() {
	GgpoGateModel g = { PHASE_NO_SESSION };
	CHECK(g.nativeBattle == NATIVE_BATTLE_OFFLINE);
	CHECK(!g.NativeExitRequired());
	CHECK(g.LocalControllerOwnsBattle(false));
	CHECK(g.MayAdvance(false, true));
}

static void TestOfflineBattleIsNeverOrphaned() {
	GgpoGateModel g = Fresh();
	g.OnSessionClosed(); // a shutdown at the menu, no battle claimed
	CHECK(!g.NativeExitRequired());
	CHECK(g.LocalControllerOwnsBattle(false));
	g.OnNativeBattleClosed();
	CHECK(!g.NativeExitRequired());
}

static void TestLosingTheSessionOrphansTheBattle() {
	GgpoGateModel g = Fresh();
	g.OnNetplayBattleClaimed();
	g.OnSessionStarted();
	g.OnRunning();
	CHECK(!g.LocalControllerOwnsBattle(true)); // GGPO owns the pad while live
	CHECK(!g.NativeExitRequired());

	g.OnSessionClosed(); // room closed, helper died, leave mid-fight
	CHECK(g.NativeExitRequired());
	CHECK(!g.LocalControllerOwnsBattle(false)); // never the local pad on both slots
	CHECK(g.MayAdvance(false, true));            // the engine must run to leave
	CHECK(g.OnOrphanFrame());                    // reported once
	CHECK(!g.OnOrphanFrame());

	g.OnNativeBattleClosed(); // CloseBattle
	CHECK(!g.NativeExitRequired());
	CHECK(g.LocalControllerOwnsBattle(false));
}

// A failure before the session starts (no endpoints, no controller, a
// refused start) aborts with no session. The claim alone must orphan.
static void TestClaimWithoutSessionOrphansOnClose() {
	GgpoGateModel g = Fresh();
	g.OnNetplayBattleClaimed();
	g.OnFatal();
	g.OnSessionClosed();
	CHECK(g.NativeExitRequired());
	// An abort never gates the battle it has to leave, nor later offline play.
	CHECK(g.MayAdvance(false, true));
	g.OnNativeBattleClosed();
	CHECK(g.MayAdvance(false, true));
	CHECK(!g.MayAdvance(false, false)); // only the manual gate remains
}

// StartGGPO retires a leftover session before starting the next one. The
// new session re-claims the battle in the same call.
static void TestLeftoverRetireThenStartReclaims() {
	GgpoGateModel g = Fresh();
	g.OnNetplayBattleClaimed();
	g.OnSessionClosed(); // leftover_before_start
	CHECK(g.NativeExitRequired());
	g.OnSessionStarted();
	CHECK(!g.NativeExitRequired());
	CHECK(g.nativeBattle == NATIVE_BATTLE_NETPLAY);
	CHECK(g.orphanFrames == 0);
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
	CHECK(!g.NativeExitRequired());
	CHECK(g.LocalControllerOwnsBattle(false));
}

static void TestMayAdvanceFollowsTheSessionWhileLive() {
	GgpoGateModel g = Fresh();
	g.OnSessionStarted();
	CHECK(!g.MayAdvance(true, true)); // waiting for RUNNING
	g.OnRunning();
	CHECK(g.MayAdvance(true, true));
	CHECK(!g.MayAdvance(true, false)); // manual pause
	g.OnFatal();
	CHECK(!g.MayAdvance(true, true));
	g.OnSessionClosed();
	CHECK(g.MayAdvance(false, true)); // the fatal state dies with the session
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
	CHECK(overdue == 1);
	CHECK(g.orphanFrames == 10);
}

static void TestClassifier() {
	CHECK(ClassifyGgpoResult(0) == POLICY_CONTINUE);
	CHECK(ClassifyGgpoResult(4) == POLICY_STALL_PREDICTION);
	CHECK(ClassifyGgpoResult(6) == POLICY_SKIP_NOT_SYNCED);
	CHECK(ClassifyGgpoResult(7) == POLICY_SKIP_OTHER);
	CHECK(ClassifyGgpoResult(8) == POLICY_SKIP_OTHER);
	CHECK(ClassifyGgpoResult(-1) == POLICY_FATAL);
	CHECK(ClassifyGgpoResult(1) == POLICY_FATAL);
	CHECK(ClassifyGgpoResult(2) == POLICY_FATAL);
	CHECK(ClassifyGgpoResult(3) == POLICY_FATAL);
	CHECK(ClassifyGgpoResult(5) == POLICY_FATAL);
	CHECK(ClassifyGgpoResult(9) == POLICY_FATAL);
	CHECK(ClassifyGgpoResult(10) == POLICY_FATAL);
	CHECK(ClassifyGgpoResult(11) == POLICY_FATAL);
	CHECK(ClassifyGgpoResult(12345) == POLICY_FATAL); // unknown codes are fatal
}

// The session must never be closed from inside a GGPO callback: the fork
// keeps calling the advance-frame callback after it returns. Outside a
// callback an abort proceeds at once; inside one it is latched and handed
// out only once every callback frame has unwound.
static void TestAbortLatchOutsideCallbackIsImmediate() {
	AbortLatch latch;
	CHECK(!latch.InCallback());
	CHECK(!latch.Request("sync failed")); // not latched: caller closes now
	CHECK(!latch.pending);
	char reason[256] = "untouched";
	CHECK(!latch.Take(reason, sizeof(reason)));
	CHECK(strcmp(reason, "untouched") == 0);
}

static void TestAbortLatchInsideCallbackDefers() {
	AbortLatch latch;
	char reason[256] = {};
	{
		AbortLatch::Scope callback(latch);
		CHECK(latch.InCallback());
		CHECK(latch.Request("first reason"));
		CHECK(latch.pending);
		// A second abort in the same burst keeps the first explanation.
		CHECK(latch.Request("second reason"));
		CHECK(strcmp(latch.reason, "first reason") == 0);
		// Still inside the callback: nothing may be taken yet.
		CHECK(!latch.Take(reason, sizeof(reason)));
		CHECK(reason[0] == '\0');
	}
	CHECK(!latch.InCallback());
	CHECK(latch.Take(reason, sizeof(reason)));
	CHECK(strcmp(reason, "first reason") == 0);
	CHECK(!latch.pending);
	CHECK(latch.reason[0] == '\0');
	CHECK(!latch.Take(reason, sizeof(reason))); // drained exactly once
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
			CHECK(latch.depth == 2);
			CHECK(latch.Request("buffer full"));
		}
		CHECK(latch.depth == 1);
		CHECK(!latch.Take(reason, sizeof(reason)));
	}
	CHECK(latch.depth == 0);
	CHECK(latch.Take(reason, sizeof(reason)));
	CHECK(strcmp(reason, "buffer full") == 0);
}

static void TestAbortLatchResetDropsPending() {
	AbortLatch latch;
	{
		AbortLatch::Scope callback(latch);
		CHECK(latch.Request("stale"));
	}
	latch.Reset(); // a new session started before the outer tick drained it
	char reason[256] = {};
	CHECK(!latch.Take(reason, sizeof(reason)));
	CHECK(!latch.pending);
}

int main() {
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
	TestMayAdvanceFollowsTheSessionWhileLive();
	TestOrphanOverdueFiresOnce();
	TestClassifier();

	if (g_failures) {
		printf("%d failure(s)\n", g_failures);
		return 1;
	}
	printf("ggpo_gate_test: all tests passed\n");
	return 0;
}
