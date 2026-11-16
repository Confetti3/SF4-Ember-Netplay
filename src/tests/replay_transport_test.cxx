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
	// The game's pause menu: what the pad does under it asks for nothing, and
	// once it closes a button held through it needs a fresh press, an RT held
	// from under it included.
	{
		PadControls paused;
		Command made[4];
		paused.Reset(0);
		CHECK(paused.Sample(xinput::RT, made, true) == 0 && paused.Sample(xinput::RT | xinput::RB, made, true) == 0);
		for (unsigned i = 0; i < kRepeatDelay * 2; i++) CHECK(paused.Sample(xinput::RT | xinput::RB, made, true) == 0);
		// The menu closes with RT and RB still down: nothing.
		for (unsigned i = 0; i < kRepeatDelay * 2; i++) CHECK(paused.Sample(xinput::RT | xinput::RB, made) == 0);
		// Let go and pressed again: the press acts.
		CHECK(paused.Sample(0, made) == 0 && paused.Sample(xinput::RT, made) == 1 && made[0] == Command::Step);
	}
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


// The commands: only the first five act on the transport, the seeks are
// their own kind.
void TestCommandKinds() {
	for (Command c : {Command::TogglePause, Command::Step, Command::Slower, Command::Faster, Command::CycleSpeed}) CHECK(MovesTransport(c) && !Seeks(c));
	for (Command c : {Command::Meter, Command::Inputs}) CHECK(!MovesTransport(c) && !Seeks(c));
	for (Command c : {Command::StepBack, Command::JumpBack, Command::JumpForward, Command::Restart, Command::Loop}) CHECK(!MovesTransport(c) && Seeks(c));
	// A step back pauses a playing replay, and leaves a paused one paused.
	Transport t;
	t.Pause(); CHECK(t.Paused() && t.Hold());
	t.Pause(); CHECK(t.Paused());
	t.Apply(Command::Step); t.Pause();
	CHECK(t.Steps() == 1);
	// The first seven keep their numbers, and a view offers no seek until a game says so.
	CHECK(static_cast<int>(Command::Inputs) == 6 && static_cast<int>(Command::StepBack) == 7);
	CHECK(!View().seek.offered && !View().seek.seeking && View().seek.loop == LoopPhase::Off);
}

void TestSeekOrders() {
	const SeekBounds bounds{101, 5000};
	Seek seek;
	// A step back is one frame earlier and pauses; a jump is five seconds.
	SeekOrder o = seek.Plan(Command::StepBack, 600, bounds);
	CHECK(o.made && o.target == 599 && o.rounding == Rounding::Earlier && o.pause);
	o = seek.Plan(Command::JumpBack, 600, bounds);
	CHECK(o.made && o.target == 300 && o.rounding == Rounding::Earlier && !o.pause);
	o = seek.Plan(Command::JumpForward, 600, bounds);
	CHECK(o.made && o.target == 900 && o.rounding == Rounding::Later && !o.pause);
	o = seek.Plan(Command::Restart, 600, bounds);
	CHECK(o.made && o.target == 101 && o.rounding == Rounding::Exact);
	// Held inside the bounds: a jump back from near the floor goes to the floor,
	// one from the floor goes nowhere (a step back there still pauses).
	o = seek.Plan(Command::JumpBack, 250, bounds);
	CHECK(o.made && o.target == 101);
	o = seek.Plan(Command::JumpBack, 101, bounds);
	CHECK(!o.made);
	o = seek.Plan(Command::StepBack, 101, bounds);
	CHECK(!o.made && o.pause);
	o = seek.Plan(Command::Restart, 101, bounds);
	CHECK(!o.made);
	// And a jump forward to the last landing, none from it.
	o = seek.Plan(Command::JumpForward, 4900, bounds);
	CHECK(o.made && o.target == 5000);
	o = seek.Plan(Command::JumpForward, 5000, bounds);
	CHECK(!o.made);
	// A seek by number: earlier or later by its own comparison, held in the bounds.
	o = seek.To(400, 600, bounds);
	CHECK(o.made && o.target == 400 && o.rounding == Rounding::Earlier && !o.pause);
	o = seek.To(9000, 600, bounds);
	CHECK(o.made && o.target == 5000 && o.rounding == Rounding::Later);
	o = seek.To(2, 600, bounds);
	CHECK(o.made && o.target == 101);
	CHECK(!seek.To(600, 600, bounds).made);
	// Commands the seek does not know make no order.
	o = seek.Plan(Command::Step, 600, bounds);
	CHECK(!o.made && !o.pause);
	// Bounds that cross (nothing to land on) still make nothing impossible.
	o = seek.Plan(Command::JumpForward, 600, SeekBounds{700, 650});
	CHECK(o.made && o.target == 700);
}

void TestSeekProgress() {
	const SeekBounds bounds{101, 5000};
	Seek seek;
	CHECK(!seek.Seeking() && !seek.Reached(9999) && seek.Landed() == 0);
	// A seek back: its restore is asked for once; it lands when the position reaches the target.
	SeekOrder o = seek.Plan(Command::JumpBack, 900, bounds);
	seek.Begin(o.target, 900);
	CHECK(seek.Seeking() && seek.Target() == 600);
	// Until the restore is made the position is the one it goes back from: not there yet.
	CHECK(!seek.Reached(900));
	CHECK(seek.TakeRestore() && !seek.TakeRestore());
	CHECK(seek.Progress(400) && !seek.Reached(599) && seek.Reached(600) && seek.Reached(640));
	seek.Land();
	CHECK(!seek.Seeking() && seek.Landed() == 1);
	// A seek forward needs no restore.
	o = seek.Plan(Command::JumpForward, 600, bounds);
	seek.Begin(o.target, 600);
	CHECK(!seek.TakeRestore());
	seek.Land();
	CHECK(seek.Landed() == 2);
	// Commands made on the way count from the target: two step backs are two frames.
	o = seek.Plan(Command::StepBack, 700, bounds);
	seek.Begin(o.target, 700);
	CHECK(seek.Target() == 699);
	o = seek.Plan(Command::StepBack, 650, bounds);
	CHECK(o.made && o.target == 698);
	seek.Begin(o.target, 650);
	CHECK(seek.Target() == 698 && !seek.TakeRestore());
	seek.Cancel();
	CHECK(!seek.Seeking() && !seek.TakeRestore() && seek.Landed() == 2);
	// A seek that stands still is given up on; one that moves is not.
	seek.Begin(900, 700);
	for (int tick = 0; tick < kSeekGiveUpTicks - 2; tick++) CHECK(seek.Progress(700));
	CHECK(seek.Progress(701));
	for (int tick = 0; tick < kSeekGiveUpTicks - 2; tick++) CHECK(seek.Progress(701));
	CHECK(!seek.Progress(701));
	// Outside a seek there is nothing to give up on.
	seek.Cancel();
	for (int tick = 0; tick < kSeekGiveUpTicks * 2; tick++) CHECK(seek.Progress(701));
	seek.Reset();
	CHECK(seek.Landed() == 0 && seek.Loop() == LoopPhase::Off);
}

void TestSeekLoop() {
	const SeekBounds bounds{101, 5000};
	Seek seek;
	// One command cycles: A here, B here and the loop on, off.
	seek.Plan(Command::Loop, 800, bounds);
	CHECK(seek.Loop() == LoopPhase::MarkedA && seek.MarkA() == 800);
	CHECK(!seek.Wrap(9000).made);
	// B on the same frame is no loop yet.
	seek.Plan(Command::Loop, 800, bounds);
	CHECK(seek.Loop() == LoopPhase::MarkedA);
	seek.Plan(Command::Loop, 1100, bounds);
	CHECK(seek.Loop() == LoopPhase::Looping && seek.MarkA() == 800 && seek.MarkB() == 1100);
	// Reaching B asks for A, exactly; not before, and not while a seek is on its way.
	CHECK(!seek.Wrap(1099).made);
	SeekOrder o = seek.Wrap(1100);
	CHECK(o.made && o.target == 800 && o.rounding == Rounding::Exact && !o.pause);
	CHECK(seek.Wrap(1103).made);
	seek.Begin(800, 1100);
	CHECK(!seek.Wrap(1100).made);
	seek.Land();
	// The loop outlives seeks, even a restart that goes before A.
	seek.Plan(Command::Restart, 900, bounds);
	CHECK(seek.Loop() == LoopPhase::Looping);
	SeekView view;
	seek.Show(view);
	CHECK(view.loop == LoopPhase::Looping && view.markA == 800 && view.markB == 1100 && view.seeks == 1 && !view.seeking);
	seek.Plan(Command::Loop, 900, bounds);
	CHECK(seek.Loop() == LoopPhase::Off && !seek.Wrap(5000).made);
	// B marked before A: the marks swap.
	seek.Plan(Command::Loop, 1500, bounds);
	seek.Plan(Command::Loop, 1200, bounds);
	CHECK(seek.Loop() == LoopPhase::Looping && seek.MarkA() == 1200 && seek.MarkB() == 1500);
	// A mark outside the bounds is held in them.
	seek.ClearLoop();
	CHECK(seek.Loop() == LoopPhase::Off);
	seek.Plan(Command::Loop, 20, bounds);
	CHECK(seek.MarkA() == 101);
}

void TestSeekPad() {
	PadControls pad;
	Command out[kMostSampled];
	// Not offered (SF4): X and the D-pad ask for nothing, alone or with the rest.
	pad.Reset(0);
	CHECK(pad.Sample(xinput::X | xinput::DPadUp | xinput::DPadDown | xinput::DPadLeft | xinput::DPadRight, out) == 0);
	CHECK(pad.Sample(0, out) == 0);
	CHECK(pad.Sample(xinput::RB | xinput::RT | xinput::LB | xinput::LT | xinput::X | xinput::DPadLeft, out) == 4);
	pad.Offer(true);
	pad.Reset(0);
	// Presses: X steps back, the D-pad jumps, restarts and loops.
	CHECK(pad.Sample(xinput::X, out) == 1 && out[0] == Command::StepBack);
	CHECK(pad.Sample(xinput::X, out) == 0);
	CHECK(pad.Sample(0, out) == 0);
	CHECK(pad.Sample(xinput::DPadLeft, out) == 1 && out[0] == Command::JumpBack);
	CHECK(pad.Sample(xinput::DPadLeft, out) == 0);
	CHECK(pad.Sample(xinput::DPadRight, out) == 1 && out[0] == Command::JumpForward);
	CHECK(pad.Sample(xinput::DPadDown, out) == 1 && out[0] == Command::Restart);
	CHECK(pad.Sample(xinput::DPadUp, out) == 1 && out[0] == Command::Loop);
	// Held, only X repeats: after 300 ms, ten times a second, as RT does.
	CHECK(pad.Sample(0, out) == 0);
	CHECK(pad.Sample(xinput::X, out) == 1);
	int repeats = 0, first = -1;
	for (int tick = 1; tick <= 60; tick++) {
		const int n = pad.Sample(xinput::X, out);
		for (int i = 0; i < n; i++) if (out[i] == Command::StepBack) { repeats++; if (first < 0) first = tick; }
	}
	CHECK(first == static_cast<int>(kRepeatDelay));
	CHECK(repeats == (60 - static_cast<int>(kRepeatDelay)) / static_cast<int>(kRepeatEvery) + 1);
	// A held X does not repeat into the next session.
	pad.Reset(xinput::X);
	for (unsigned i = 0; i < kRepeatDelay * 2; i++) CHECK(pad.Sample(xinput::X, out) == 0);
	// Under the game's pause menu nothing is asked, and what is held then is not a press once it closes.
	pad.Reset(0);
	CHECK(pad.Sample(xinput::X | xinput::DPadUp, out, true) == 0);
	CHECK(pad.Sample(xinput::X | xinput::DPadUp, out) == 0);
	CHECK(pad.Sample(0, out) == 0 && pad.Sample(xinput::DPadUp, out) == 1 && out[0] == Command::Loop);
	// Everything at once, in the order the controls are listed, within the room given.
	pad.Reset(0);
	const int n = pad.Sample(xinput::RB | xinput::RT | xinput::LB | xinput::LT | xinput::X | xinput::DPadUp | xinput::DPadDown | xinput::DPadLeft | xinput::DPadRight, out);
	CHECK(n == kMostSampled);
	CHECK(out[4] == Command::StepBack && out[5] == Command::JumpBack && out[6] == Command::JumpForward && out[7] == Command::Restart && out[8] == Command::Loop);
	// The D-pad bits are not any button's, and come from the pad's wButtons.
	CHECK(xinput::Directions(0x000F) == (xinput::DPadUp | xinput::DPadDown | xinput::DPadLeft | xinput::DPadRight));
	CHECK(xinput::Directions(0x0001) == xinput::DPadUp && xinput::Directions(0x0004) == xinput::DPadLeft);
	CHECK(xinput::Directions(0xFFF0) == 0 && xinput::Physical(0x000F) == 0);
	for (std::uint32_t d : {xinput::DPadUp, xinput::DPadDown, xinput::DPadLeft, xinput::DPadRight})
		for (std::uint32_t b : {xinput::View, xinput::Start, xinput::Y, xinput::B, xinput::A, xinput::X, xinput::LB, xinput::RB, xinput::LT, xinput::RT, xinput::LS, xinput::RS}) CHECK((d & b) == 0);
}

void TestSeekKeys() {
	for (unsigned key : {kKeyComma, kKeyLeft, kKeyRight, kKeyHome, kKeyL}) {
		CHECK(SeekKey(key) && !PlaybackKey(key));
		// A game that does not seek keeps none of them from the game.
		CHECK(!KeepsKey(true, kKeyDown, key) && !KeepsKey(true, kKeyUp, key));
		CHECK(KeepsKey(true, kKeyDown, key, true) && KeepsKey(true, kKeyUp, key, true));
		CHECK(!KeepsKey(false, kKeyDown, key, true));
		CHECK(!KeepsKey(true, 0x104, key, true) && !KeepsKey(true, 0x105, key, true));
	}
	// The playback keys stay kept either way, and nothing else is.
	CHECK(KeepsKey(true, kKeyDown, kKeyF1, true) && KeepsKey(true, kKeyDown, kKeyF1));
	for (unsigned key : {0x41u, 0x26u, 0x28u, 0x0Du, 0xBDu}) CHECK(!KeepsKey(true, kKeyDown, key, true));
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
	TestCommandKinds();
	TestSeekOrders();
	TestSeekProgress();
	TestSeekLoop();
	TestSeekPad();
	TestSeekKeys();
	return 0;
}
