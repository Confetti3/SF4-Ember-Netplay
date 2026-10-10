// The replay controls' rules (common/ReplayTransport.hxx): the cadence at
// each speed, pause and step, the game's own Select folded into the speed,
// the round reset, when the controls may act, the meter fed only on frames
// that moved on, and the pad and keyboard mappings.

#include "../common/ReplayTransport.hxx"

#include <cstring>
#include <initializer_list>

#include "test_support.hxx"

using namespace sf4e::replaytransport;
namespace xinput = sf4e::input::xinput;

namespace {

// Releases (calls that play a frame) in `calls` cadence calls.
int Releases(Transport& transport, int calls) {
	int released = 0;
	for (int i = 0; i < calls; i++) if (!transport.Hold()) released++;
	return released;
}

void TestCadence() {
	Transport t;
	CHECK(!t.Paused() && t.Divisor() == 1);
	CHECK(Releases(t, 240) == 240);
	t.Apply(Command::Slower);
	CHECK(t.Divisor() == 2);
	// Half speed is the game's own: release, hold, release, hold.
	for (int i = 0; i < 8; i++) CHECK(t.Hold() == (i % 2 == 1));
	CHECK(Releases(t, 240) == 120);
	t.Apply(Command::Slower);
	CHECK(t.Divisor() == 4);
	for (int i = 0; i < 8; i++) CHECK(t.Hold() == (i % 4 != 0));
	CHECK(Releases(t, 240) == 60);
	t.Apply(Command::Slower);
	CHECK(t.Divisor() == 4);
	// A change of speed releases on the very next call.
	t.Hold();
	t.Apply(Command::Faster);
	CHECK(t.Divisor() == 2 && !t.Hold() && t.Hold());
	t.Apply(Command::Faster);
	CHECK(t.Divisor() == 1);
	t.Apply(Command::Faster);
	CHECK(t.Divisor() == 1);
	// LB cycles 1x, 1/2, 1/4 and back.
	t.Apply(Command::CycleSpeed); CHECK(t.Divisor() == 2);
	t.Apply(Command::CycleSpeed); CHECK(t.Divisor() == 4);
	t.Apply(Command::CycleSpeed); CHECK(t.Divisor() == 1);
}

void TestPauseAndStep() {
	Transport t;
	t.Apply(Command::TogglePause);
	CHECK(t.Paused() && Releases(t, 240) == 0);
	// Each step plays exactly one frame, and only once.
	t.Apply(Command::Step);
	CHECK(!t.Hold() && t.Hold() && t.Hold());
	t.Apply(Command::Step); t.Apply(Command::Step); t.Apply(Command::Step);
	CHECK(t.Steps() == 3 && Releases(t, 10) == 3 && t.Steps() == 0);
	// Steps wait in a bounded queue.
	for (unsigned i = 0; i < kMostSteps + 10; i++) t.Apply(Command::Step);
	CHECK(t.Steps() == kMostSteps && Releases(t, 100) == static_cast<int>(kMostSteps));
	// A speed change while paused stays paused.
	t.Apply(Command::Slower);
	CHECK(t.Paused() && t.Divisor() == 2 && Releases(t, 20) == 0);
	// Resuming drops waiting steps and plays the next call.
	t.Apply(Command::Step); t.Apply(Command::Step);
	t.Apply(Command::TogglePause);
	CHECK(!t.Paused() && t.Steps() == 0 && !t.Hold() && t.Hold());
	// Step while playing pauses without playing a frame.
	Transport playing;
	playing.Apply(Command::Step);
	CHECK(playing.Paused() && playing.Steps() == 0 && playing.Hold());
	// Meter and Inputs leave the transport alone.
	playing.Apply(Command::Meter); playing.Apply(Command::Inputs);
	CHECK(playing.Paused() && playing.Divisor() == 1);
	CHECK(MovesTransport(Command::TogglePause) && MovesTransport(Command::Step) && MovesTransport(Command::CycleSpeed) &&
		!MovesTransport(Command::Meter) && !MovesTransport(Command::Inputs));
}

void TestNativeSelectFolds() {
	Transport t;
	// Nothing changed from what was written: nothing to fold.
	t.FoldNative(false);
	CHECK(t.Divisor() == 1 && !t.SlowFlag());
	// Select at 1x: the game set its flag, which is a request for 1/2.
	t.FoldNative(true);
	CHECK(t.Divisor() == 2 && t.SlowFlag());
	// Ember goes to 1/4; the flag it writes stays set and is not a press.
	t.Apply(Command::Slower);
	CHECK(t.SlowFlag());
	t.FoldNative(true);
	CHECK(t.Divisor() == 4);
	// Select at 1/4: the game cleared its flag, a request for 1x.
	t.FoldNative(false);
	CHECK(t.Divisor() == 1 && !t.SlowFlag());
	// Ember's own speed change is not undone by the flag it wrote last.
	t.Apply(Command::Slower);
	t.FoldNative(false);
	CHECK(t.Divisor() == 2 && t.SlowFlag());
	// Ember cleared the flag (a new round, an export): no press either.
	t.Reset(); t.Wrote(false);
	t.FoldNative(false);
	CHECK(t.Divisor() == 1);
	// A reset keeps what the flag holds: a flag Ember left set is not a press.
	t.Apply(Command::CycleSpeed);
	CHECK(t.SlowFlag());
	t.Reset();
	t.FoldNative(true);
	CHECK(t.Divisor() == 1 && !t.SlowFlag());
	// Folding pauses nothing.
	t.Apply(Command::TogglePause);
	t.FoldNative(true);
	CHECK(t.Paused() && t.Divisor() == 2);
}

void TestRoundReset() {
	Transport t;
	t.Apply(Command::TogglePause); t.Apply(Command::Slower); t.Apply(Command::Slower); t.Apply(Command::Step);
	t.Reset();
	CHECK(!t.Paused() && t.Divisor() == 1 && t.Steps() == 0 && !t.Hold());
	CHECK(RoundBoundary(kFlowRoundStart) && RoundBoundary(kFlowReady) && !RoundBoundary(kFlowFight));
	// The knockout, the round's end and the result are not round starts.
	for (int flow : {3, 4, 5, 6, 15}) CHECK(!RoundBoundary(flow));
}

Context Replay() {
	Context c;
	c.recorderMode = kRecorderPlaying; c.fight = true;
	return c;
}

void TestGuard() {
	CHECK(Decide(Replay()) == Route::Transport);
	CHECK(Playback(Replay()));
	// Netplay, recording (Versus, netplay), stopped and Training: the game's own.
	Context c = Replay(); c.netplay = true;
	CHECK(Decide(c) == Route::Original && !Playback(c));
	c = Replay(); c.recorderMode = 2;
	CHECK(Decide(c) == Route::Original && !Playback(c));
	c = Replay(); c.recorderMode = 0;
	CHECK(Decide(c) == Route::Original);
	c = Replay(); c.training = true;
	CHECK(Decide(c) == Route::Original && !Playback(c));
	// Out of the fight (the intro, the knockout, the round's end): the game's
	// cadence, which clears the freeze bit, so they play at 1x.
	c = Replay(); c.fight = false;
	CHECK(Decide(c) == Route::Original && Playback(c));
	// Under the pause menu.
	c = Replay(); c.pauseMenu = true;
	CHECK(Decide(c) == Route::Original);
	// An export: the game's cadence at 1x, in the fight and out of it.
	c = Replay(); c.exporting = true;
	CHECK(Decide(c) == Route::FullSpeed);
	c.fight = false;
	CHECK(Decide(c) == Route::FullSpeed);
	// An export flag never touches netplay.
	c.netplay = true;
	CHECK(Decide(c) == Route::Original);
}

void TestAdvanceGate() {
	AdvanceGate gate;
	CHECK(gate.Advanced(100));
	CHECK(!gate.Advanced(100) && !gate.Advanced(100));
	CHECK(gate.Advanced(101));
	CHECK(gate.Advanced(0xFFFF) && gate.Advanced(0));
	gate.Reset();
	CHECK(gate.Advanced(0));
}

// The battle's frame counter moves only on a call that released; the meter
// is fed once per outer frame through the gate. It must see every frame
// that moved on, once, and each one after the last, at every speed and
// through pause and steps.
void TestMeterFedOnlyOnAdvance() {
	Transport t;
	AdvanceGate gate;
	std::uint16_t frame = 500;
	int fed = 0, advanced = 0;
	std::uint16_t last = frame;
	bool any = false;
	const auto outerFrame = [&] {
		if (!t.Hold()) { ++frame; ++advanced; }
		if (gate.Advanced(frame)) {
			if (any) CHECK(static_cast<std::uint16_t>(frame - last) == 1);
			any = true; last = frame; ++fed;
		}
	};
	gate.Advanced(frame); // the frame the meter already shows
	for (int i = 0; i < 60; i++) outerFrame();
	t.Apply(Command::Slower); t.Apply(Command::Slower);
	for (int i = 0; i < 60; i++) outerFrame();
	t.Apply(Command::TogglePause);
	for (int i = 0; i < 60; i++) outerFrame();
	for (int i = 0; i < 5; i++) { t.Apply(Command::Step); outerFrame(); outerFrame(); outerFrame(); }
	t.Apply(Command::TogglePause); t.Apply(Command::Faster);
	for (int i = 0; i < 60; i++) outerFrame();
	CHECK(fed == advanced && advanced == 60 + 15 + 0 + 5 + 30);
}

// One battle update, in the order the game runs its tasks: SYS MAIN runs the
// cadence, which holds or releases; CMD POST moves the recorder's cursor on
// a released frame; then Ember observes. What it observes is the cursor and
// the frame that update reached, on release, step and pause alike.
void TestObservedAfterUpdate() {
	Transport t;
	AdvanceGate gate;
	const std::uint32_t frames[kRounds] = {900, 900};
	std::uint32_t cursor = 0;
	std::uint16_t simulated = 100;
	gate.Advanced(simulated);
	const auto update = [&] {
		const bool hold = t.Hold();               // SYS MAIN: the cadence
		if (!hold) { ++cursor; ++simulated; }     // CMD POST: the cursor moves
		return ObserveUpdate(gate, 0, cursor, frames, simulated);  // after the update
	};
	for (int i = 0; i < 10; i++) {
		const auto seen = update();
		CHECK(seen.cursor == cursor && seen.advanced && seen.roundFrames[1] == 900);
	}
	t.Apply(Command::TogglePause);
	const std::uint32_t paused = cursor;
	for (int i = 0; i < 10; i++) { const auto seen = update(); CHECK(seen.cursor == paused && !seen.advanced); }
	t.Apply(Command::Step);
	auto seen = update();
	CHECK(seen.cursor == paused + 1 && seen.advanced);
	seen = update();
	CHECK(seen.cursor == paused + 1 && !seen.advanced);
	t.Apply(Command::TogglePause); t.Apply(Command::Slower);
	for (int i = 0; i < 8; i++) { seen = update(); CHECK(seen.cursor == cursor && seen.advanced == (i % 2 == 0)); }
}

// The game's cadence function as the design reads it: with the half speed
// flag set it toggles the freeze bit each call; without, it clears it.
void NativeCadence(unsigned& flags, int slow) {
	if (slow) flags ^= kReplayFreeze;
	else flags &= ~kReplayFreeze;
}

// At 1/2 or 1/4 the pause menu opens and the game's cadence takes over: the
// freeze bit is released on every call of it, the speed asked for is kept,
// and it is written again when Ember's cadence resumes. Select pressed under
// the pause menu stays the game's.
void TestPauseMenuReleasesFreeze() {
	for (const int slower : {1, 2}) {
		Transport t;
		unsigned flags = 0;
		int slow = 0;
		for (int i = 0; i < slower; i++) t.Apply(Command::Slower);
		// Ember's cadence in the fight.
		for (int i = 0; i < 6; i++) {
			t.FoldNative(slow != 0);
			if (t.Hold()) flags |= kReplayFreeze; else flags &= ~kReplayFreeze;
			slow = t.SlowFlag() ? 1 : 0;
		}
		CHECK(slow == 1);
		// The pause menu: the game's cadence, handed back each call.
		flags |= kPauseOrHold;
		for (int i = 0; i < 7; i++) {
			Relinquish(t, slow);
			NativeCadence(flags, slow);
			CHECK((flags & kReplayFreeze) == 0 && slow == 0);
		}
		CHECK(t.Divisor() == (slower == 1 ? 2 : 4));
		// Closed again: Ember's cadence writes the speed back.
		flags &= ~kPauseOrHold;
		t.FoldNative(slow != 0);
		CHECK(t.Divisor() == (slower == 1 ? 2 : 4) && t.SlowFlag());
	}
	// Select under the pause menu sets the game's half speed: it is the game's.
	Transport t;
	int slow = 1;
	Relinquish(t, slow);
	CHECK(slow == 1);
	t.FoldNative(slow != 0);
	CHECK(t.Divisor() == 2);
}

// Commands are the playback session's they were made under: one posted under
// an earlier session, even after it ended, never acts on the next.
void TestSessionQueue() {
	CommandQueue queue;
	queue.Submit(Command::TogglePause, Device::Keyboard, 1);
	queue.Submit(Command::Step, Device::Pad, 1);
	auto taken = queue.Take(1);
	CHECK(taken.size() == 2 && taken[0].command == Command::TogglePause && taken[1].device == Device::Pad);
	// A key pressed over an older snapshot arrives after the next session began.
	queue.Submit(Command::Step, Device::Keyboard, 1);
	queue.Submit(Command::Faster, Device::Keyboard, 2);
	taken = queue.Take(2);
	CHECK(taken.size() == 1 && taken[0].command == Command::Faster);
	CHECK(queue.Take(1).empty());
	for (std::size_t i = 0; i < CommandQueue::kMost + 4; i++) queue.Submit(Command::Step, Device::Pad, 3);
	queue.Clear();
	CHECK(queue.Take(3).empty());
	for (std::size_t i = 0; i < CommandQueue::kMost + 4; i++) queue.Submit(Command::Step, Device::Pad, 3);
	CHECK(queue.Take(3).size() == CommandQueue::kMost);
	// A held RT does not repeat into the next session: the pad starts over.
	PadControls pad;
	Command out[4];
	pad.Reset(0);
	pad.Sample(xinput::RT, out);
	for (unsigned i = 0; i < kRepeatDelay - 1; i++) pad.Sample(xinput::RT, out);
	pad.Reset(xinput::RT);
	for (unsigned i = 0; i < kRepeatDelay * 2; i++) CHECK(pad.Sample(xinput::RT, out) == 0);
}

void TestPad() {
	PadControls pad;
	Command out[4];
	// Start, View (the game's Select), A, B, X and Y ask for nothing.
	CHECK(pad.Sample(xinput::Start | xinput::View | xinput::A | xinput::B | xinput::X | xinput::Y, out) == 0);
	CHECK(pad.Sample(0, out) == 0);
	// Presses, not holds.
	CHECK(pad.Sample(xinput::RB, out) == 1 && out[0] == Command::TogglePause);
	CHECK(pad.Sample(xinput::RB, out) == 0);
	CHECK(pad.Sample(0, out) == 0);
	CHECK(pad.Sample(xinput::LB, out) == 1 && out[0] == Command::CycleSpeed);
	CHECK(pad.Sample(xinput::LB | xinput::LT, out) == 1 && out[0] == Command::Inputs);
	CHECK(pad.Sample(0, out) == 0);
	// RT steps on the press, then again after 300 ms, ten times a second.
	CHECK(pad.Sample(xinput::RT, out) == 1 && out[0] == Command::Step);
	int steps = 0, firstRepeat = -1;
	for (int tick = 1; tick <= 60; tick++) {
		const int n = pad.Sample(xinput::RT, out);
		if (n) { CHECK(n == 1 && out[0] == Command::Step); steps++; if (firstRepeat < 0) firstRepeat = tick; }
	}
	CHECK(firstRepeat == static_cast<int>(kRepeatDelay));
	CHECK(steps == 1 + (60 - static_cast<int>(kRepeatDelay)) / static_cast<int>(kRepeatEvery));
	// Released and pressed again: one step, the delay starts over.
	CHECK(pad.Sample(0, out) == 0 && pad.Sample(xinput::RT, out) == 1);
	CHECK(pad.Sample(xinput::RT, out) == 0);
	// Several at once.
	pad.Reset();
	CHECK(pad.Sample(xinput::RB | xinput::RT | xinput::LB | xinput::LT, out) == 4);
	CHECK(out[0] == Command::TogglePause && out[1] == Command::Step && out[2] == Command::CycleSpeed && out[3] == Command::Inputs);
	// Buttons held when the playback starts are not presses.
	pad.Reset(xinput::RB | xinput::LT);
	CHECK(pad.Sample(xinput::RB | xinput::LT, out) == 0);
	CHECK(pad.Sample(0, out) == 0 && pad.Sample(xinput::RB, out) == 1);
}

void TestKeys() {
	for (unsigned key : {kKeyF1, kKeyF2, kKeyF3, kKeyF4, kKeyF5, kKeyF9}) {
		CHECK(KeepsKey(true, kKeyDown, key) && KeepsKey(true, kKeyUp, key));
		// Only while a replay plays.
		CHECK(!KeepsKey(false, kKeyDown, key));
		// With Alt they are system keys and reach the game: Alt+F4 closes it.
		CHECK(!KeepsKey(true, 0x104, key) && !KeepsKey(true, 0x105, key));
	}
	// F6 to F8 are Training's, F10 the shell's, F11 and F12 nobody's here.
	for (unsigned key : {0x75u, 0x76u, 0x77u, 0x79u, 0x7Au, 0x7Bu, 0x41u, 0x0Du}) CHECK(!KeepsKey(true, kKeyDown, key));
}

void TestStrip() {
	CHECK(StripAlpha(true, 1, -1) == 1 && StripAlpha(false, 2, -1) == 1 && StripAlpha(false, 4, 100) == 1);
	CHECK(StripAlpha(false, 1, -1) == 0);
	CHECK(StripAlpha(false, 1, 0) == 1 && StripAlpha(false, 1, 3) == 1);
	CHECK(StripAlpha(false, 1, 3.25) > .49f && StripAlpha(false, 1, 3.25) < .51f);
	CHECK(StripAlpha(false, 1, 3.5) == 0 && StripAlpha(false, 1, 60) == 0);
	char clock[16];
	FormatClock(0, clock); CHECK(!std::strcmp(clock, "0:00.00"));
	// 2477 frames: 41 seconds and 17 frames, 28 hundredths.
	FormatClock(2477, clock); CHECK(!std::strcmp(clock, "0:41.28"));
	FormatClock(59, clock); CHECK(!std::strcmp(clock, "0:00.98"));
	FormatClock(60 * 60 * 61 + 30, clock); CHECK(!std::strcmp(clock, "61:00.50"));
	FormatClock(0xFFFFFFFFu, clock); CHECK(std::strlen(clock) < sizeof(clock));
}

}

int main() {
	TestCadence();
	TestPauseAndStep();
	TestNativeSelectFolds();
	TestRoundReset();
	TestGuard();
	TestAdvanceGate();
	TestMeterFedOnlyOnAdvance();
	TestObservedAfterUpdate();
	TestPauseMenuReleasesFreeze();
	TestSessionQueue();
	TestPad();
	TestKeys();
	TestStrip();
	return 0;
}
