// Pure unit tests for the match's battle step (BattleStep.hxx).

#include "../common/BattleStep.hxx"

#include "test_support.hxx"

using namespace sf4e::battlestep;

// FrameRate=SMOOTH: the match steps one frame at a time, then the player's
// setting comes back.
static void TestSmoothIsFixedForTheMatch() {
	MatchStep step;
	CHECK(step.Begin(0.0f));
	CHECK(step.Saved() == 0.0f);
	CHECK(step.End());
	CHECK(step.Saved() == 0.0f);
	// Closed: a second close puts nothing back.
	CHECK(!step.End());
}

// FrameRate=FIXED already steps one frame: nothing to change or put back.
static void TestFixedIsLeftAlone() {
	MatchStep step;
	CHECK(!step.Begin(OneFrame));
	CHECK(!step.End());
}

// A step above 0 moves the battle that many frames, so it is fixed too.
static void TestOtherStepsAreFixed() {
	MatchStep step;
	CHECK(step.Begin(1.5f));
	CHECK(step.End() && step.Saved() == 1.5f);
}

// A session restarted before its battle closes keeps the player's value,
// not the forced one.
static void TestRestartKeepsThePlayersSetting() {
	MatchStep step;
	CHECK(step.Begin(0.0f));
	CHECK(!step.Begin(OneFrame));
	CHECK(step.End() && step.Saved() == 0.0f);
}

// A battle that closes without a match puts nothing back.
static void TestCloseWithoutMatch() {
	MatchStep step;
	CHECK(!step.End());
}

int main() {
	TestSmoothIsFixedForTheMatch();
	TestFixedIsLeftAlone();
	TestOtherStepsAreFixed();
	TestRestartKeepsThePlayersSetting();
	TestCloseWithoutMatch();
	printf("battle_step_test: all tests passed\n");
	return 0;
}
