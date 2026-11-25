// Pure unit tests for the battle load watch and the line that says what loads.

#include "../common/BattleLoadFacts.hxx"
#include "../common/LoadWatch.hxx"

#include <string>

#include "test_support.hxx"

using namespace sf4e::loadwatch;

static void TestHealthyLoadSaysNothing() {
	LoadWatch watch;
	CHECK(!watch.Tick(1000));
	watch.LoadStarted(1000);
	CHECK(!watch.Tick(1000 + FirstStallMs - 1));
	CHECK(!watch.FirstFrame(1000 + FirstStallMs - 1));
	// Finished: later ticks have nothing to report.
	CHECK(!watch.Tick(1000 + SecondStallMs * 2));
}

static void TestStallReportsOncePerThreshold() {
	LoadWatch watch;
	watch.LoadStarted(5000);
	auto report = watch.Tick(5000 + FirstStallMs);
	CHECK(report && report->kind == ReportKind::StalledAt20s);
	CHECK(report->elapsedMs == FirstStallMs);
	CHECK(!watch.Tick(5000 + FirstStallMs + 1));
	CHECK(!watch.Tick(5000 + SecondStallMs - 1));
	report = watch.Tick(5000 + SecondStallMs);
	CHECK(report && report->kind == ReportKind::StillStalledAt60s);
	CHECK(!watch.Tick(5000 + SecondStallMs * 3));
}

static void TestLateTickReportsTheTwentySecondStallFirst() {
	LoadWatch watch;
	watch.LoadStarted(0);
	auto report = watch.Tick(SecondStallMs + 5);
	CHECK(report && report->kind == ReportKind::StalledAt20s);
	report = watch.Tick(SecondStallMs + 6);
	CHECK(report && report->kind == ReportKind::StillStalledAt60s);
}

static void TestSlowLoadThatFinishesIsRecovered() {
	LoadWatch watch;
	watch.LoadStarted(100);
	CHECK(watch.Tick(100 + FirstStallMs));
	auto report = watch.FirstFrame(100 + 30000);
	CHECK(report && report->kind == ReportKind::Recovered);
	CHECK(report->elapsedMs == 30000);
	CHECK(!watch.FirstFrame(100 + 31000));

	watch.LoadStarted(0);
	CHECK(watch.Tick(FirstStallMs));
	CHECK(watch.Tick(SecondStallMs));
	report = watch.FirstFrame(SecondStallMs + 4000);
	CHECK(report && report->kind == ReportKind::Recovered && report->elapsedMs == SecondStallMs + 4000);
}

static void TestCloseEndsTheWatch() {
	LoadWatch watch;
	watch.LoadStarted(0);
	watch.BattleClosed(10);
	CHECK(!watch.Tick(SecondStallMs));
	watch.LoadStarted(0);
	CHECK(watch.Tick(FirstStallMs));
	watch.BattleClosed(FirstStallMs + 1);
	CHECK(!watch.Tick(SecondStallMs));
	CHECK(!watch.FirstFrame(SecondStallMs));
}

static void TestNewLoadRestartsTheWatch() {
	LoadWatch watch;
	watch.LoadStarted(0);
	CHECK(watch.Tick(FirstStallMs));
	watch.LoadStarted(FirstStallMs + 100);
	CHECK(!watch.Tick(FirstStallMs + 200));
	auto report = watch.Tick(FirstStallMs + 100 + FirstStallMs);
	CHECK(report && report->kind == ReportKind::StalledAt20s);
}

static void TestClockGoingBackwardsIsNotAStall() {
	LoadWatch watch;
	watch.LoadStarted(50000);
	CHECK(!watch.Tick(10));
	CHECK(!watch.FirstFrame(10));
}

static void TestLoadLine() {
	BattleLoadFacts facts;
	CHECK(FormatBattleLoad(facts) == "mode=native select, fighters and stage not read");
	facts.emberSetUp = true;
	CHECK(FormatBattleLoad(facts) == "mode=unread, fighters and stage not read");
	facts.mode = LoadMode::Versus;
	facts.sides[0] = { 1, 2, 3, 1, 4 };
	facts.sides[1] = { 5, 6, 7, 0, 8 };
	facts.stage = 9;
	// Ember's own loads keep the line they had.
	CHECK(FormatBattleLoad(facts) ==
		"mode=versus P1 fighter=1 costume=2 color=3 ultra=1 edition=4, P2 fighter=5 costume=6 color=7 ultra=0 edition=8, stage=9");
	facts.mode = LoadMode::Training;
	CHECK(FormatBattleLoad(facts).rfind("mode=training P1 fighter=1", 0) == 0);
	// The game's own select reads into the same line, one grep for both.
	facts.emberSetUp = false;
	facts.stage.reset();
	CHECK(FormatBattleLoad(facts) ==
		"mode=native training P1 fighter=1 costume=2 color=3 ultra=1 edition=4, P2 fighter=5 costume=6 color=7 ultra=0 edition=8, stage=not read");
}

int main() {
	TestHealthyLoadSaysNothing();
	TestStallReportsOncePerThreshold();
	TestLateTickReportsTheTwentySecondStallFirst();
	TestSlowLoadThatFinishesIsRecovered();
	TestCloseEndsTheWatch();
	TestNewLoadRestartsTheWatch();
	TestClockGoingBackwardsIsNotAStall();
	TestLoadLine();
	return 0;
}
