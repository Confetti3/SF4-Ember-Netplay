// Pure unit tests for the time-sync pacing controller.

#include <stdio.h>

#include "../common/sf4e__PacingController.hxx"

using namespace sf4e::pacing;

static int g_failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
			g_failures++;                                                    \
		}                                                                    \
	} while (0)

static bool Near(double a, double b) {
	double d = a - b;
	return d > -1e-9 && d < 1e-9;
}

static PacingController Fresh() {
	PacingController p;
	p.InitDefaults();
	return p;
}

static void TestClamping() {
	PacingController p = Fresh();

	// Negative and zero recommendations are ignored safely.
	p.OnRecommendation(0);
	p.OnRecommendation(-5);
	CHECK(Near(p.outstandingMs, 0.0));
	CHECK(p.recommendationsReceived == 2);
	CHECK(p.framesRecommendedTotal == 0);

	// A 3-frame recommendation converts to 50 ms.
	p.OnRecommendation(3);
	CHECK(Near(p.outstandingMs, 50.0));

	// A huge recommendation clamps to the 9-frame cap (150 ms).
	p.OnRecommendation(1000);
	CHECK(Near(p.outstandingMs, 150.0));
	CHECK(Near(p.maxOutstandingMs, 150.0));
}

static void TestRepeatedRecommendationsDoNotAccumulate() {
	PacingController p = Fresh();

	// Recommendations are fresh estimates: they replace, never sum.
	p.OnRecommendation(9);
	p.OnRecommendation(9);
	p.OnRecommendation(9);
	CHECK(Near(p.outstandingMs, 150.0));
	CHECK(Near(p.msReplacedTotal, 300.0));

	// A smaller fresh estimate lowers the target.
	p.OnRecommendation(3);
	CHECK(Near(p.outstandingMs, 50.0));
}

static void TestEnabledDisabledABGate() {
	PacingController p = Fresh();
	p.enabled = false;
	p.OnRecommendation(3);
	CHECK(p.recommendationsReceived == 1);
	CHECK(Near(p.outstandingMs, 0.0));
	CHECK(Near(p.msAcceptedTotal, 0.0));
	CHECK(Near(p.msDiscardedDisabled, 50.0));
	CHECK(Near(p.NextWaitMs(), 0.0));

	p.enabled = true;
	p.OnRecommendation(3);
	CHECK(Near(p.outstandingMs, 50.0));
	CHECK(Near(p.NextWaitMs(), 3.0));
}

static void TestWaitResultAccounting() {
	PacingController p = Fresh();
	p.OnRecommendation(3);
	p.OnWaitRequested(3.0);
	p.OnWaited(3.25);
	p.OnFallbackSleep();
	CHECK(p.waitRequests == 1);
	CHECK(Near(p.msRequestedTotal, 3.0));
	CHECK(Near(p.maxRequestedWaitMs, 3.0));
	CHECK(Near(p.msAppliedTotal, 3.25));
	CHECK(p.fallbackSleeps == 1);

	p.OnWaitFailure(false);
	p.OnWaitFailure(true);
	CHECK(p.waitFailures == 2);
	CHECK(p.waitTimeouts == 1);
}

static void TestPerFrameBudgetAndRepayment() {
	PacingController p = Fresh();
	p.OnRecommendation(3); // 50 ms

	// Each outer frame repays at most maxStepMs.
	CHECK(Near(p.NextWaitMs(), 3.0));

	// Full repayment loop terminates and total-applied ≈ debt.
	int iterations = 0;
	while (p.NextWaitMs() > 0.0 && iterations < 1000) {
		p.OnWaited(p.NextWaitMs());
		iterations++;
	}
	CHECK(iterations >= 16 && iterations <= 17); // 50/3
	CHECK(p.outstandingMs < p.minWaitMs);
	CHECK(p.msAppliedTotal > 49.0 && p.msAppliedTotal < 51.0);
	CHECK(Near(p.maxSingleWaitMs, 3.0));
}

static void TestOverWaitClampsToZero() {
	PacingController p = Fresh();
	p.OnRecommendation(3); // 50 ms

	// Coarse timers can over-wait; debt must not go negative.
	p.OnWaited(80.0);
	CHECK(Near(p.outstandingMs, 0.0));
	CHECK(Near(p.NextWaitMs(), 0.0));

	// Bogus non-positive waits are ignored.
	p.OnRecommendation(3);
	p.OnWaited(0.0);
	p.OnWaited(-5.0);
	CHECK(Near(p.outstandingMs, 50.0));
}

static void TestMinGranularity() {
	PacingController p = Fresh();
	p.OnRecommendation(3);
	// Wait everything down to below minWaitMs.
	p.OnWaited(49.5);
	CHECK(p.outstandingMs > 0.0 && p.outstandingMs < 1.0);
	// Residual debt below the minimum wait is not worth a kernel wait.
	CHECK(Near(p.NextWaitMs(), 0.0));
}

static void TestReset() {
	PacingController p = Fresh();
	p.OnRecommendation(9); // 150 ms
	p.OnWaited(3.0);
	CHECK(p.outstandingMs > 0.0);

	// No correction survives the match lifecycle; discard is recorded.
	p.Reset();
	CHECK(Near(p.outstandingMs, 0.0));
	CHECK(Near(p.msDiscardedOnReset, 147.0));
	CHECK(Near(p.NextWaitMs(), 0.0));

	// Stats reset separately (per-match).
	p.ResetStats();
	CHECK(p.recommendationsReceived == 0);
	CHECK(Near(p.msDiscardedOnReset, 0.0));
}

static void TestConfigurableCaps() {
	PacingController p = Fresh();
	p.maxRecommendationFrames = 4.0;
	p.maxStepMs = 1.5;
	p.OnRecommendation(9);
	CHECK(Near(p.outstandingMs, 4.0 * 1000.0 / 60.0));
	CHECK(Near(p.NextWaitMs(), 1.5));
}

int main() {
	TestClamping();
	TestRepeatedRecommendationsDoNotAccumulate();
	TestEnabledDisabledABGate();
	TestWaitResultAccounting();
	TestPerFrameBudgetAndRepayment();
	TestOverWaitClampsToZero();
	TestMinGranularity();
	TestReset();
	TestConfigurableCaps();

	if (g_failures) {
		printf("%d failure(s)\n", g_failures);
		return 1;
	}
	printf("pacing_controller_test: all tests passed\n");
	return 0;
}
