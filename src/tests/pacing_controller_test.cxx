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
	CHECK(Near(p.NextShiftMs(), 0.0));

	p.enabled = true;
	p.OnRecommendation(3);
	CHECK(Near(p.outstandingMs, 50.0));
	CHECK(Near(p.NextShiftMs(), 3.0));
}

static void TestContinuousIgnoresRecommendations() {
	PacingController p = Fresh();
	p.continuous = true;
	p.OnRecommendation(3);
	CHECK(p.recommendationsReceived == 1);
	CHECK(Near(p.outstandingMs, 0.0));
}

static void TestShiftAccounting() {
	PacingController p = Fresh();
	p.OnRecommendation(3);
	p.OnShiftApplied(3.25);
	CHECK(Near(p.msSlowedTotal, 3.25));
	CHECK(Near(p.outstandingMs, 50.0 - 3.25));
	p.OnShiftApplied(-1.0);
	CHECK(Near(p.msSpedUpTotal, 1.0));
	CHECK(Near(p.outstandingMs, 50.0 - 2.25));
	CHECK(Near(p.maxSingleShiftMs, 3.25));
}

static void TestPerFrameBudgetAndRepayment() {
	PacingController p = Fresh();
	p.OnRecommendation(3); // 50 ms

	// Each outer frame repays at most maxStepMs.
	CHECK(Near(p.NextShiftMs(), 3.0));

	// Full repayment loop terminates and total-applied ≈ debt.
	int iterations = 0;
	while (p.NextShiftMs() > 0.0 && iterations < 1000) {
		p.OnShiftApplied(p.NextShiftMs());
		iterations++;
	}
	CHECK(iterations >= 16 && iterations <= 17); // 50/3
	CHECK(p.outstandingMs < p.minShiftMs);
	CHECK(p.msSlowedTotal > 49.0 && p.msSlowedTotal < 51.0);
	CHECK(Near(p.maxSingleShiftMs, 3.0));
}

static void TestAppliedMovesDebtExactly() {
	PacingController p = Fresh();
	p.OnRecommendation(3); // 50 ms

	// A shift that lands after the target moved still counts in full, even
	// across zero, so the next shift pays it back.
	p.OnShiftApplied(80.0);
	CHECK(Near(p.outstandingMs, -30.0));
	CHECK(Near(p.NextShiftMs(), -3.0));
	p.OnShiftApplied(0.0);
	CHECK(Near(p.outstandingMs, -30.0));
}

static void TestMinGranularity() {
	PacingController p = Fresh();
	p.OnRecommendation(3);
	// Wait everything down to below minWaitMs.
	p.OnShiftApplied(49.5);
	CHECK(p.outstandingMs > 0.0 && p.outstandingMs < 1.0);
	// Residual debt below the minimum is not worth a shift.
	CHECK(Near(p.NextShiftMs(), 0.0));
}

static void TestReset() {
	PacingController p = Fresh();
	p.OnRecommendation(9); // 150 ms
	p.OnShiftApplied(3.0);
	CHECK(p.outstandingMs > 0.0);

	// No correction survives the match lifecycle; discard is recorded.
	p.Reset();
	CHECK(Near(p.outstandingMs, 0.0));
	CHECK(Near(p.msDiscardedOnReset, 147.0));
	CHECK(Near(p.NextShiftMs(), 0.0));

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
	CHECK(Near(p.NextShiftMs(), 1.5));
}

int main() {
	TestClamping();
	TestRepeatedRecommendationsDoNotAccumulate();
	TestEnabledDisabledABGate();
	TestContinuousIgnoresRecommendations();
	TestShiftAccounting();
	TestPerFrameBudgetAndRepayment();
	TestAppliedMovesDebtExactly();
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
