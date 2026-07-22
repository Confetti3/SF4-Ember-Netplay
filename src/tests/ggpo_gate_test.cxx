// Pure unit tests for the GGPO simulation gate model and error classifier.

#include <stdio.h>

#include "../common/sf4e__GgpoGate.hxx"

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

int main() {
	TestLifecycleGating();
	TestConnectionWarningDoesNotBlock();
	TestWarningPlusManualPause();
	TestThresholdPlusResume();
	TestFatalImmuneToResume();
	TestNewSessionClearsPerSessionStateButKeepsPause();
	TestRunningOnlyFromWaiting();
	TestClassifier();

	if (g_failures) {
		printf("%d failure(s)\n", g_failures);
		return 1;
	}
	printf("ggpo_gate_test: all tests passed\n");
	return 0;
}
