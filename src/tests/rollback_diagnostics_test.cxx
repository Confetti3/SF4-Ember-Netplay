// Pure unit tests for the rollback diagnostics component (no game deps).

#include <stdio.h>
#include <string.h>

#include "../common/sf4e__RollbackDiagnostics.hxx"

using namespace sf4e::diag;

static int g_failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
			g_failures++;                                                    \
		}                                                                    \
	} while (0)

static void TestHistogramAccounting() {
	TimingStat s;
	s.Reset();
	CHECK(s.ApproxPercentileMs(0.5) == 0.0);

	s.Record(0.1);   // bucket 0 (<0.25)
	s.Record(0.3);   // bucket 1 (<0.5)
	s.Record(0.75);  // bucket 2 (<1)
	s.Record(1.5);   // bucket 3 (<2)
	s.Record(3.0);   // bucket 4 (<4)
	s.Record(6.0);   // bucket 5 (<8)
	s.Record(10.0);  // bucket 6 (<16.67)
	s.Record(20.0);  // bucket 7 (<33)
	s.Record(40.0);  // bucket 8 (<50)
	s.Record(120.0); // bucket 9 (open)

	CHECK(s.count == 10);
	for (int i = 0; i < NUM_BUCKETS; i++) {
		CHECK(s.buckets[i] == 1);
	}
	CHECK(s.maxMs == 120.0);
	CHECK(s.lastMs == 120.0);
	CHECK(s.totalMs > 201.0 && s.totalMs < 202.0);
	CHECK(s.hitchCounts[0] == 3); // 20, 40, 120
	CHECK(s.hitchCounts[1] == 2); // 40, 120
	CHECK(s.hitchCounts[2] == 2);
	CHECK(s.hitchCounts[3] == 1);
	CHECK(s.hitchCounts[4] == 1);

	// Negative durations clamp to zero rather than corrupting totals.
	TimingStat n;
	n.Reset();
	n.Record(-5.0);
	CHECK(n.count == 1 && n.totalMs == 0.0 && n.buckets[0] == 1);
}

static void TestPercentiles() {
	TimingStat s;
	s.Reset();
	for (int i = 0; i < 99; i++) {
		s.Record(0.1); // all in bucket 0
	}
	s.Record(40.0); // one in bucket 8

	// p50 falls in bucket 0 → its upper bound.
	CHECK(s.ApproxPercentileMs(0.50) == 0.25);
	CHECK(s.ApproxPercentileMs(0.95) == 0.25);
	// p99+ hits the outlier's bucket upper bound (50.0).
	CHECK(s.ApproxPercentileMs(0.995) == 50.0);

	// Open-ended top bucket reports observed max, never infinity.
	TimingStat t;
	t.Reset();
	t.Record(200.0);
	CHECK(t.ApproxPercentileMs(0.5) == 200.0);
	CHECK(t.ApproxPercentileMs(1.0) == 200.0);
}

static void TestBurstTracking() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(1000.0);

	// Burst of 3 rollback callbacks, ended by a normal advance.
	d.OnRollbackCallback(1001.0);
	d.OnRollbackCallback(1001.1);
	d.OnRollbackCallback(1001.2);
	d.OnFrameAdvanced(1002.0);
	CHECK(d.maxResimBurst == 3);
	CHECK(d.resimBursts == 1);
	CHECK(d.resimBurstBuckets[2] == 1); // bucket for burst length 3

	// Burst of 1 ended by outer frame boundary.
	d.OnRollbackCallback(1003.0);
	d.OnOuterFrame(1004.0);
	CHECK(d.maxResimBurst == 3);
	CHECK(d.resimBursts == 2);
	CHECK(d.resimBurstBuckets[0] == 1); // bucket for burst length 1

	CHECK(d.totalRollbackCallbacks == 4);
	CHECK(d.outerFramesWithRollback == 1);
	CHECK(d.maxRollbackCallbacksPerOuterFrame == 4);
	CHECK(d.outerFrames == 1);
}

static void TestStallAccounting() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(0.0);

	// Two consecutive refused frames form ONE stall episode.
	d.RecordSkip(SKIP_PREDICTION_THRESHOLD, 100.0);
	d.RecordSkip(SKIP_PREDICTION_THRESHOLD, 116.0);
	CHECK(d.predictionStalls == 1);
	CHECK(d.skipReasons[SKIP_PREDICTION_THRESHOLD] == 2);

	d.OnFrameAdvanced(150.0);
	CHECK(d.predictionStallDuration.count == 1);
	CHECK(d.predictionStallDuration.maxMs == 50.0);
	CHECK(d.predictionStallStartMs < 0.0);

	// A later stall is a new episode.
	d.RecordSkip(SKIP_PREDICTION_THRESHOLD, 200.0);
	CHECK(d.predictionStalls == 2);
}

static void TestStallSpansOuterTicks() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(0.0);

	// Exact production sequence: outer application ticks do not imply
	// deterministic progression and therefore cannot split the episode.
	d.RecordSkip(SKIP_PREDICTION_THRESHOLD, 100.0);
	d.OnOuterFrame(110.0);
	d.RecordSkip(SKIP_PREDICTION_THRESHOLD, 120.0);
	d.OnOuterFrame(130.0);
	d.RecordSkip(SKIP_PREDICTION_THRESHOLD, 140.0);
	d.OnFrameAdvanced(175.0);

	CHECK(d.predictionStalls == 1);
	CHECK(d.skipReasons[SKIP_PREDICTION_THRESHOLD] == 3);
	CHECK(d.predictionStallDuration.count == 1);
	CHECK(d.predictionStallDuration.maxMs == 75.0);
	CHECK(d.predictionStallStartMs < 0.0);
}

static void TestSessionEndClosesActiveStall() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(0.0);
	d.RecordSkip(SKIP_PREDICTION_THRESHOLD, 25.0);
	d.OnSessionEnded(125.0);

	CHECK(d.predictionStalls == 1);
	CHECK(d.predictionStallDuration.count == 1);
	CHECK(d.predictionStallDuration.maxMs == 100.0);
	CHECK(d.predictionStallStartMs < 0.0);
}

static void TestConnectionWarningAccounting() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(0.0);

	d.OnConnectionInterrupted(100.0);
	d.OnConnectionInterrupted(110.0); // duplicate event: one episode
	CHECK(d.connectionWarnings == 1);
	d.OnConnectionResumed(300.0);
	CHECK(d.connectionResumes == 1);
	CHECK(d.connectionWarningDuration.count == 1);
	CHECK(d.connectionWarningDuration.maxMs == 200.0);

	// Terminal disconnect during a warning also closes the episode.
	d.OnConnectionInterrupted(400.0);
	d.OnTerminalDisconnect(450.0);
	CHECK(d.terminalDisconnects == 1);
	CHECK(d.connectionWarningDuration.count == 2);
}

static void TestGgpoResultSlots() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(0.0);

	d.RecordGgpoResult(CALL_ADD_LOCAL_INPUT, 0);   // GGPO_OK → slot 1
	d.RecordGgpoResult(CALL_ADD_LOCAL_INPUT, 4);   // PREDICTION_THRESHOLD → slot 5
	d.RecordGgpoResult(CALL_ADD_LOCAL_INPUT, -1);  // general failure → slot 0
	d.RecordGgpoResult(CALL_ADD_LOCAL_INPUT, 99);  // out of range → slot 0
	CHECK(d.ggpoResults[CALL_ADD_LOCAL_INPUT][1] == 1);
	CHECK(d.ggpoResults[CALL_ADD_LOCAL_INPUT][5] == 1);
	CHECK(d.ggpoResults[CALL_ADD_LOCAL_INPUT][0] == 2);
}

static void TestTimesyncAccounting() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(0.0);

	d.OnTimesyncEvent(3);
	d.OnTimesyncEvent(9);
	d.OnTimesyncEvent(0);  // counted, but adds no frames
	d.OnTimesyncEvent(-2); // negative ignored for frame totals
	CHECK(d.timesyncEvents == 4);
	CHECK(d.timesyncFramesTotal == 12);
	CHECK(d.timesyncMaxFrames == 9);
}

static void TestResetAndDisabled() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(0.0);
	d.RecordOp(OP_SAVE_TOTAL, 1.0);
	CHECK(d.ops[OP_SAVE_TOTAL].count == 1);

	d.ResetForMatch(500.0);
	CHECK(d.ops[OP_SAVE_TOTAL].count == 0);
	CHECK(d.enabled); // reset preserves enablement
	CHECK(d.matchStartMs == 500.0);

	SetEnabled(false);
	d.RecordOp(OP_SAVE_TOTAL, 1.0);
	d.OnRollbackCallback(1.0);
	d.RecordSkip(SKIP_UPDATE_GATE, 1.0);
	CHECK(d.ops[OP_SAVE_TOTAL].count == 0);
	CHECK(d.totalRollbackCallbacks == 0);
	CHECK(d.skipReasons[SKIP_UPDATE_GATE] == 0);
	char buf[64];
	CHECK(d.FormatSummary(buf, sizeof(buf), "x") == 0);
}

static void TestPeriodicDue() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(0.0);
	CHECK(!d.PeriodicSummaryDue(5000.0, 10.0));
	CHECK(d.PeriodicSummaryDue(10000.0, 10.0));
	CHECK(!d.PeriodicSummaryDue(10001.0, 10.0)); // latched
	CHECK(d.PeriodicSummaryDue(20001.0, 10.0));
}

static void TestFormatSummary() {
	RollbackDiagnostics& d = G();
	SetEnabled(true);
	d.ResetForMatch(0.0);
	d.RecordOp(OP_SAVE_TOTAL, 1.5);
	d.RecordOp(OP_GGPO_IDLE, 0.1);
	d.RecordSkip(SKIP_UPDATE_GATE, 1.0);
	d.OnRollbackCallback(1.0);
	d.OnOuterFrame(2.0);
	d.RecordGgpoResult(CALL_SYNC_INPUT, 0);
	d.trackedKeys.Update(88);

	char buf[8192];
	size_t n = d.FormatSummary(buf, sizeof(buf), "test");
	CHECK(n > 0 && n < sizeof(buf));
	CHECK(strstr(buf, "hard stalls") != NULL);
	CHECK(strstr(buf, "simulation hitches") != NULL);
	CHECK(strstr(buf, "visible corrections") != NULL);
	CHECK(strstr(buf, "proxy") != NULL); // burst is labeled a proxy
	CHECK(strstr(buf, "save") != NULL);
	CHECK(strstr(buf, "skip[update_gate]=1") != NULL);
	CHECK(strstr(buf, "trackedKeys=88/88") != NULL);
	CHECK(strstr(buf, "hitch[>=16.67,25,33.33,50,100]") != NULL);

	// A tiny buffer must not overflow and stays terminated.
	char tiny[16];
	size_t tn = d.FormatSummary(tiny, sizeof(tiny), "test");
	CHECK(tn < sizeof(tiny));
	CHECK(tiny[sizeof(tiny) - 1] == '\0' || strlen(tiny) < sizeof(tiny));
}

int main() {
	TestHistogramAccounting();
	TestPercentiles();
	TestBurstTracking();
	TestStallAccounting();
	TestStallSpansOuterTicks();
	TestSessionEndClosesActiveStall();
	TestConnectionWarningAccounting();
	TestGgpoResultSlots();
	TestTimesyncAccounting();
	TestResetAndDisabled();
	TestPeriodicDue();
	TestFormatSummary();

	if (g_failures) {
		printf("%d failure(s)\n", g_failures);
		return 1;
	}
	printf("rollback_diagnostics_test: all tests passed\n");
	return 0;
}
