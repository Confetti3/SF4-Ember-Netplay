// The replay takeover's rules (common/ReplayTakeover.hxx): when the timeline
// opens, what each command does at each stage and what the game thread is
// asked for, that the stage moves only once the game thread carried a step,
// what a failed step leaves, the countdown, and how a takeover ends.

#include "../common/ReplayTakeover.hxx"

#include "test_support.hxx"

using namespace sf4e::replaytakeover;

namespace {

// Opened from a pause, Start asked and carried.
Takeover Begun(int side = 0) {
	Takeover t;
	t.Apply(Command::TakeOver, true, true);
	if (side) t.Apply(Command::Side, true, true);
	t.Apply(Command::Start, true, true);
	t.Carried(true);
	return t;
}

void TestOpenOnlyPaused() {
	Takeover t;
	CHECK(t.GetStage() == Stage::Off && t.Pending() == Act::None);
	// A playing replay keeps playing: the timeline opens from a pause only.
	CHECK(t.Apply(Command::TakeOver, false, true) == Act::None && t.GetStage() == Stage::Off);
	// Watching, the takeover's other commands are not taken.
	for (Command c : {Command::Start, Command::Restart, Command::Timeline, Command::Side}) CHECK(t.Apply(c, true, true) == Act::None && t.GetStage() == Stage::Off && t.Side() == 0);
	CHECK(t.Apply(Command::TakeOver, true, true) == Act::None && t.GetStage() == Stage::Choosing);
	CHECK(t.Accepts(Command::Step) && t.Accepts(Command::StepBack) && !t.Accepts(Command::TogglePause));
	for (Command c : {Command::TakeOver, Command::Restart, Command::Timeline, Command::TogglePause}) CHECK(t.Apply(c, true, true) == Act::None && t.GetStage() == Stage::Choosing);
}

void TestStartWaitsForTheGame() {
	Takeover t;
	t.Apply(Command::TakeOver, true, true);
	t.Apply(Command::Side, true, true);
	CHECK(t.Side() == 1);
	// A jump still playing to its frame is not a frame yet.
	CHECK(t.Apply(Command::Start, true, false) == Act::None && t.Pending() == Act::None);
	CHECK(t.Apply(Command::Start, true, true) == Act::Begin);
	// Nothing moves until the game thread has saved the frame, and nothing else is taken meanwhile.
	CHECK(t.GetStage() == Stage::Choosing && t.Pending() == Act::Begin && !t.Hold());
	CHECK(t.Apply(Command::Side, true, true) == Act::None && t.Side() == 1);
	CHECK(t.Apply(Command::Start, true, true) == Act::None);
	// The timeline's own steps and jumps wait too, so the frame saved is the one shown.
	for (Command c : {Command::Step, Command::StepBack, Command::SecondBack, Command::SecondAhead, Command::Watch}) CHECK(!t.Accepts(c));
	t.Carried(true);
	CHECK(t.GetStage() == Stage::Countdown && t.Countdown() == kCountdownTicks && t.Pending() == Act::None);
	// Taken, the side stays as picked.
	CHECK(t.Apply(Command::Side, true, true) == Act::None && t.Side() == 1);
}

void TestFailedStartKeepsTheTimeline() {
	Takeover t;
	t.Apply(Command::TakeOver, true, true);
	t.Apply(Command::Start, true, true);
	t.Carried(false);
	// The frame could not be saved: the timeline stays open over the paused replay; leaving it is an End, which is safe with no state.
	CHECK(t.GetStage() == Stage::Choosing && t.Pending() == Act::None);
	CHECK(t.End() == Act::End && t.End() == Act::None);
}

void TestCountdown() {
	Takeover t = Begun();
	// The battle holds 3 s, then plays: the fighter moves on the 180th tick's frame.
	int held = 0;
	while (t.Hold()) held++;
	CHECK(held == kCountdownTicks - 1 && t.GetStage() == Stage::Playing && t.Countdown() == 0);
	CHECK(!t.Hold() && t.GetStage() == Stage::Playing);
	CHECK(CountdownSeconds(180) == 3 && CountdownSeconds(121) == 3 && CountdownSeconds(120) == 2 && CountdownSeconds(60) == 1 && CountdownSeconds(1) == 1 && CountdownSeconds(0) == 0);
	Takeover off;
	CHECK(!off.Hold());
}

void TestRestartAndTimeline() {
	Takeover t = Begun(1);
	while (t.Hold()) {}
	// Restart: a load, then the countdown again, paused or not.
	CHECK(t.Apply(Command::Restart, false, false) == Act::Restart);
	CHECK(t.GetStage() == Stage::Playing && t.Apply(Command::Timeline, false, true) == Act::None);
	t.Carried(true);
	CHECK(t.GetStage() == Stage::Countdown && t.Countdown() == kCountdownTicks && t.Side() == 1);
	// Restart during the countdown starts it over.
	t.Hold(); t.Hold();
	CHECK(t.Apply(Command::Restart, false, true) == Act::Restart);
	t.Carried(true);
	CHECK(t.Countdown() == kCountdownTicks);
	// Timeline: back to the frame with the side the replay's, to pick again.
	CHECK(t.Apply(Command::Timeline, false, true) == Act::Return);
	t.Carried(true);
	CHECK(t.GetStage() == Stage::Choosing && t.Countdown() == 0);
	// A new start saves a new frame.
	CHECK(t.Apply(Command::Start, true, true) == Act::Begin);
}

void TestFailedLoadEnds() {
	// A load that failed has left the battle: the takeover is over, nothing left to end.
	for (Command c : {Command::Restart, Command::Timeline}) {
		Takeover t = Begun();
		while (t.Hold()) {}
		t.Apply(c, false, true);
		t.Carried(false);
		CHECK(t.GetStage() == Stage::Off && t.Pending() == Act::None && t.End() == Act::None);
	}
}

void TestWatchAndEnd() {
	Takeover t;
	// Nothing to end.
	CHECK(t.End() == Act::None);
	// Watch closes the timeline, begun or not.
	t.Apply(Command::TakeOver, true, true);
	CHECK(t.Apply(Command::Watch, true, true) == Act::End && t.GetStage() == Stage::Off);
	// Watch only from the timeline; after a takeover it frees the save state.
	t = Begun();
	CHECK(t.Apply(Command::Watch, true, true) == Act::None && t.GetStage() == Stage::Countdown);
	t.Apply(Command::Timeline, true, true); t.Carried(true);
	CHECK(t.Apply(Command::Watch, true, true) == Act::End && t.GetStage() == Stage::Off && t.End() == Act::None);
	// A round boundary ends a takeover at any stage.
	t = Begun();
	while (t.Hold()) {}
	CHECK(t.End() == Act::End && t.GetStage() == Stage::Off);
	CHECK(t.End() == Act::None);
	// A step waiting when it ends goes with it.
	t = Begun();
	t.Apply(Command::Restart, true, true);
	CHECK(t.End() == Act::End && t.Pending() == Act::None);
	t.Carried(true);
	CHECK(t.GetStage() == Stage::Off);
}

}

int main() {
	TestOpenOnlyPaused();
	TestStartWaitsForTheGame();
	TestFailedStartKeepsTheTimeline();
	TestCountdown();
	TestRestartAndTimeline();
	TestFailedLoadEnds();
	TestWatchAndEnd();
	return 0;
}
