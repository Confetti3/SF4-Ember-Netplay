#pragma once

// Pause, frame step and slower speeds for a replay the game's Battle Log
// plays, without Dimps: the transport, when it may act, and the rules the
// game thread (sf4e__ReplayPlayback.cxx) and the overlay share.
//
// How it acts, read from the code (REPLAY_PLAYBACK_CONTROLS_DESIGN): the
// game's own half speed is the replay cadence function (0x5D7750), which
// sets the freeze bit of the battle's simulation flags on one call and
// clears it on the next. With the bit set, SysMain_UpdatePauseState suspends
// the simulation tasks, the replay cursor among them, as the pause menu
// does. Ember replaces that function during playback: Hold() says, once a
// call, whether the bit is set. A call without it plays one frame.
//
// A game that can also seek (SFV Ember: Seek, below) offers its commands
// through the same transport: step back, jump five seconds either way,
// restart and an A-B loop. SF4 offers none of them and never sends one.
//
// ReplayTransportTest covers every rule here.

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

#include "PadKind.hxx"

namespace sf4e { namespace replaytransport {

// The recorder's modes (ReplayRecorder.hxx: RecorderMode); 1 plays a replay back.
constexpr int kRecorderPlaying = 1;
// The recorder's round streams (ReplayRecorder.hxx: kStreams).
constexpr int kRounds = 7;
// Battle flows (Dimps::Game::Battle::System::BattleFlow) the transport
// reads; sf4e__ReplayPlayback.cxx checks they agree.
constexpr int kFlowReady = 1, kFlowFight = 2, kFlowRoundStart = 14;
// The simulation flags' pause menu or hold bit, and the freeze bit the
// cadence drives (System::SystemSimulationFlags).
constexpr unsigned kPauseOrHold = 0x1, kReplayFreeze = 0x8;

// What the player can ask for. The first five act on the transport and
// only in the fight; Meter and Inputs show or hide a layer at any point
// of the playback; the last five seek (Seek, below) and are only sent by a
// game that offers them (SeekView::offered).
enum class Command : std::uint8_t { TogglePause, Step, Slower, Faster, CycleSpeed, Meter, Inputs,
	StepBack, JumpBack, JumpForward, Restart, Loop };
inline bool MovesTransport(Command command) {
	return command == Command::TogglePause || command == Command::Step || command == Command::Slower ||
		command == Command::Faster || command == Command::CycleSpeed;
}
inline bool Seeks(Command command) { return command >= Command::StepBack; }
// Where a command came from, for the glyphs the strip shows.
enum class Device : std::uint8_t { Keyboard, Pad };

// What the cadence call sees. Read only what is needed: nothing but
// `netplay` and `recorderMode` is read outside a replay's playback.
struct Context {
	bool netplay = false;     // a GGPO session exists
	int recorderMode = 0;     // ReplaySystem +0x708
	bool training = false;    // the battle is Training
	bool exporting = false;   // the playback is being made into a video
	bool fight = false;       // IsFight (0x5D9F60)
	bool pauseMenu = false;   // kPauseOrHold is set
};

// A replay is playing back: the game reports it and nothing else owns the
// battle. Netplay, Versus and Training never are.
inline bool Playback(const Context& context) {
	return !context.netplay && context.recorderMode == kRecorderPlaying && !context.training;
}

// What the cadence call does.
//   Original: the game's own function, unchanged.
//   FullSpeed: an export. The game's function with its half speed cleared
//              first, so the video always plays at 1x.
//   Transport: Ember's cadence; the controls act.
// Ember's cadence only runs in the fight with the pause menu closed. The
// game's own drives the knockout, the round's end and the pause menu, after
// Ember hands it back (Relinquish), so all of those play at 1x.
enum class Route { Original, FullSpeed, Transport };
inline Route Decide(const Context& context) {
	if (!Playback(context)) return Route::Original;
	if (context.exporting) return Route::FullSpeed;
	if (!context.fight || context.pauseMenu) return Route::Original;
	return Route::Transport;
}

// A new round is being set up: the transport starts it playing at 1x, so a
// pause left on at the end of a round cannot freeze the next one on its
// first frame of the fight.
inline bool RoundBoundary(int flow) { return flow == kFlowRoundStart || flow == kFlowReady; }

// The most single frames that can wait to be stepped.
constexpr unsigned kMostSteps = 30;

class Transport {
public:
	bool Paused() const { return paused_; }
	// 1, 2 or 4: one frame in that many calls.
	int Divisor() const { return divisor_; }
	unsigned Steps() const { return steps_; }

	// Playing at 1x, nothing waiting. The half speed last written is kept:
	// it is what the game's flag still holds.
	void Reset() { paused_ = false; divisor_ = 1; steps_ = 0; phase_ = 0; }

	// Pauses a playing replay (a step back lands paused).
	void Pause() { if (!paused_) { paused_ = true; steps_ = 0; phase_ = 0; } }

	// Step pauses a playing replay; paused, it queues one frame.
	void Apply(Command command) {
		switch (command) {
		case Command::TogglePause: paused_ = !paused_; steps_ = 0; phase_ = 0; break;
		case Command::Step: if (!paused_) { paused_ = true; phase_ = 0; } else if (steps_ < kMostSteps) ++steps_; break;
		case Command::Slower: SetDivisor(divisor_ == 1 ? 2 : 4); break;
		case Command::Faster: SetDivisor(divisor_ == 4 ? 2 : 1); break;
		case Command::CycleSpeed: SetDivisor(divisor_ == 1 ? 2 : divisor_ == 2 ? 4 : 1); break;
		default: break;
		}
	}

	// Once per cadence call: whether the freeze bit is set for it. A call
	// that releases plays one frame; at 1/N one call in N releases, starting
	// with the first after a change, and N = 2 is the game's own half speed.
	bool Hold() {
		if (paused_) {
			if (!steps_) return true;
			--steps_;
			return false;
		}
		const bool release = phase_ == 0;
		phase_ = (phase_ + 1) % static_cast<unsigned>(divisor_);
		return !release;
	}

	// The game's half speed flag as this call found it. The game flips it on
	// a Select press before the cadence runs, so a change from what Ember
	// last wrote is that press: to 1, from 1x, asks for 1/2; to 0, from a
	// slower speed, asks for 1x. Select keeps its meaning, and its legend
	// never disagrees with Ember's speed.
	void FoldNative(bool slow) {
		if (slow == written_) return;
		if (slow && divisor_ == 1) SetDivisor(2);
		else if (!slow && divisor_ != 1) SetDivisor(1);
	}
	// The flag to write back this call: set at any slower speed.
	bool SlowFlag() { written_ = divisor_ != 1; return written_; }
	// Ember wrote the flag outside the transport (cleared it).
	void Wrote(bool slow) { written_ = slow; }
	bool Written() const { return written_; }

private:
	void SetDivisor(int divisor) { if (divisor != divisor_) { divisor_ = divisor; phase_ = 0; } }
	bool paused_ = false, written_ = false;
	int divisor_ = 1;
	unsigned steps_ = 0, phase_ = 0;
};

// What a game that seeks publishes for the strip's timeline and chips. Frames
// of the playback's own timeline (Seek, below).
enum class LoopPhase : std::uint8_t { Off, MarkedA, Looping };
struct SeekView {
	bool offered = false;    // the game takes the seek commands: the strip shows their chips
	bool seeking = false;    // a seek is on its way to its landing
	std::uint32_t position = 0, total = 0;
	std::uint32_t floor = 0, last = 0;   // the earliest and the latest landing now
	LoopPhase loop = LoopPhase::Off;
	std::uint32_t markA = 0, markB = 0;  // the loop's marks (A set, then both)
	std::uint32_t seeks = 0;             // seeks that landed
};

// Seeking, game-neutral: positions are frames of the playback's own timeline
// (SFV: the number of frames the session has played). The game says where a
// landing is legal, between a floor and a last, and which positions inside
// them are (the Rounding of an order: a frame the game cannot land on is
// rounded the way the order says); this keeps what the player asked for: the
// target of a seek on its way, and an A-B loop's marks.
//
// A step back lands paused (SeekOrder::pause); the other seeks keep the
// transport as it is. Commands that arrive while a seek is on its way count
// from its target, so two quick step backs are two frames.
constexpr int kJumpFrames = 300;          // five seconds at 60 a second
constexpr unsigned kMostSeekFrames = 30;  // battle frames played per engine frame while seeking
constexpr int kSeekGiveUpTicks = 600;     // engine frames a seek may stand still before it is dropped
enum class Rounding : std::uint8_t { Exact, Earlier, Later };

struct SeekBounds { int floor = 0, last = 0; };
struct SeekOrder {
	bool made = false;       // a seek to `target` is asked for
	int target = 0;          // within the bounds, to be rounded by `rounding`
	Rounding rounding = Rounding::Exact;
	bool pause = false;      // the transport pauses first, whether or not a seek is made
};

class Seek {
public:
	void Reset() { *this = Seek(); }

	// A seek command (Command, Seeks) at `position` within `bounds`. Loop makes
	// no order: it cycles the marks (A here, then B here and the loop on, then off).
	SeekOrder Plan(Command command, int position, const SeekBounds& bounds) {
		SeekOrder order;
		const int base = seeking_ ? target_ : position;
		switch (command) {
		case Command::StepBack: order.pause = true; Aim(order, base - 1, Rounding::Earlier, base, bounds); break;
		case Command::JumpBack: Aim(order, base - kJumpFrames, Rounding::Earlier, base, bounds); break;
		case Command::JumpForward: Aim(order, base + kJumpFrames, Rounding::Later, base, bounds); break;
		case Command::Restart: Aim(order, bounds.floor, Rounding::Exact, base, bounds); break;
		case Command::Loop: CycleLoop(Clamp(base, bounds)); break;
		default: break;
		}
		return order;
	}
	// A seek to `target` asked for by number (a test tool's), earlier or later
	// than where the playback stands by its own comparison.
	SeekOrder To(int target, int position, const SeekBounds& bounds) const {
		SeekOrder order;
		const int base = seeking_ ? target_ : position;
		Aim(order, target, target < base ? Rounding::Earlier : Rounding::Later, base, bounds);
		return order;
	}
	// Once each engine frame: a loop that has reached its B asks for A.
	SeekOrder Wrap(int position) const {
		SeekOrder order;
		if (loop_ == LoopPhase::Looping && !seeking_ && position >= b_) { order.made = true; order.target = a_; }
		return order;
	}
	// The game rounded an order to a legal landing: the seek is on its way.
	void Begin(int landing, int position) {
		target_ = landing; seeking_ = true; restore_ = landing < position;
		lastPosition_ = position; stalled_ = 0;
	}
	// A seek that goes back first needs a state restored: true once.
	bool TakeRestore() { const bool wanted = restore_; restore_ = false; return wanted; }
	// The playback is at `position`: false when a seek has stood still too long.
	bool Progress(int position) {
		if (!seeking_) return true;
		if (position != lastPosition_) { lastPosition_ = position; stalled_ = 0; }
		return ++stalled_ < kSeekGiveUpTicks;
	}
	bool Seeking() const { return seeking_; }
	int Target() const { return target_; }
	bool Reached(int position) const { return seeking_ && position >= target_; }
	void Land() { seeking_ = restore_ = false; ++landed_; }
	void Cancel() { seeking_ = restore_ = false; }
	// The playback left the span the loop's marks are in.
	void ClearLoop() { loop_ = LoopPhase::Off; }

	LoopPhase Loop() const { return loop_; }
	int MarkA() const { return a_; }
	int MarkB() const { return b_; }
	unsigned Landed() const { return landed_; }
	// The fields the strip shows that are the model's.
	void Show(SeekView& out) const {
		out.seeking = seeking_; out.loop = loop_;
		out.markA = static_cast<std::uint32_t>(a_); out.markB = static_cast<std::uint32_t>(b_);
		out.seeks = landed_;
	}

private:
	static int Clamp(int at, const SeekBounds& bounds) { return (std::min)((std::max)(at, bounds.floor), (std::max)(bounds.floor, bounds.last)); }
	// An order for `target` held in the bounds, made when it moves from `base`
	// the way the order's rounding goes.
	static void Aim(SeekOrder& order, int target, Rounding rounding, int base, const SeekBounds& bounds) {
		order.target = Clamp(target, bounds); order.rounding = rounding;
		order.made = rounding == Rounding::Earlier ? order.target < base : rounding == Rounding::Later ? order.target > base : order.target != base;
	}
	void CycleLoop(int at) {
		switch (loop_) {
		case LoopPhase::Off: loop_ = LoopPhase::MarkedA; a_ = at; break;
		// B here; marked the other way round, the marks swap. On the same frame there is no loop yet.
		case LoopPhase::MarkedA: if (at != a_) { b_ = (std::max)(a_, at); a_ = (std::min)(a_, at); loop_ = LoopPhase::Looping; } break;
		case LoopPhase::Looping: loop_ = LoopPhase::Off; break;
		}
	}
	bool seeking_ = false, restore_ = false;
	int target_ = 0, lastPosition_ = 0, stalled_ = 0;
	LoopPhase loop_ = LoopPhase::Off;
	int a_ = 0, b_ = 0;
	unsigned landed_ = 0;
};

// The frame meter is shown a frame only when the battle moved on: a held
// call leaves the frames-simulated counter where it was, and the meter
// starts over on any frame number that does not follow the last.
class AdvanceGate {
public:
	bool Advanced(std::uint16_t frame) {
		if (seen_ && frame == last_) return false;
		seen_ = true; last_ = frame;
		return true;
	}
	void Reset() { seen_ = false; }
private:
	bool seen_ = false;
	std::uint16_t last_ = 0;
};

// Commands from any thread for the cadence, each with the playback session
// it was made under (View::session). Bounded: the cadence does not run
// outside a battle, and nothing waits that long. Take hands over, in order,
// only the commands of the session that plays now; one made under an earlier
// session, even posted after that session ended, is dropped.
class CommandQueue {
public:
	struct Entry { Command command; Device device; std::uint32_t session; };
	static constexpr std::size_t kMost = 16;
	void Submit(Command command, Device device, std::uint32_t session) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (entries_.size() < kMost) entries_.push_back({command, device, session});
	}
	std::vector<Entry> Take(std::uint32_t session) {
		std::vector<Entry> taken;
		{ std::lock_guard<std::mutex> lock(mutex_); taken.swap(entries_); }
		taken.erase(std::remove_if(taken.begin(), taken.end(), [session](const Entry& entry) { return entry.session != session; }), taken.end());
		return taken;
	}
	void Clear() { std::lock_guard<std::mutex> lock(mutex_); entries_.clear(); }
private:
	std::mutex mutex_;
	std::vector<Entry> entries_;
};

// Handing the cadence back to the game's own function (Route::Original):
// with its half speed flag set, that function toggles the freeze bit from
// call to call instead of clearing it, so a half speed Ember wrote is taken
// back first and the game's function clears the bit; that part plays at 1x.
// The transport keeps the speed the player asked for and writes it again
// when Ember's cadence resumes. A half speed the player set with Select
// outside it is the game's, and stays.
inline void Relinquish(Transport& transport, int& slow) {
	if (!transport.Written()) return;
	slow = 0; transport.Wrote(false);
}

// What the playback observes of the replay once a battle update is over: the
// recorder's round and cursor and its rounds' frames as that update left
// them, the cursor already moved on by the update's CMD POST task, and
// whether the battle moved on, for the frame meter (AdvanceGate). Taken after
// the update, never at the cadence call, which runs before CMD POST.
struct Observation {
	int round = 0;
	std::uint32_t cursor = 0;
	std::uint32_t roundFrames[kRounds] = {};
	bool advanced = false;
};
inline Observation ObserveUpdate(AdvanceGate& gate, int round, std::uint32_t cursor, const std::uint32_t (&roundFrames)[kRounds], std::uint16_t simulated) {
	Observation seen;
	seen.round = round; seen.cursor = cursor;
	for (int i = 0; i < kRounds; i++) seen.roundFrames[i] = roundFrames[i];
	seen.advanced = gate.Advanced(simulated);
	return seen;
}

// The pad's controls, from its physical XInput buttons sampled once a game
// tick: RB pause or play, RT step (held, again after kRepeatDelay ticks and
// every kRepeatEvery after), LB the next speed, LT the input lanes. Start and
// Select stay the game's. A game that seeks also offers (Offer) X, the step
// back, which repeats as RT does, and the D-pad: Left and Right jump five
// seconds back and forward, Down restarts, Up cycles the loop.
constexpr unsigned kRepeatDelay = 18, kRepeatEvery = 6; // 300 ms, then 10 a second
// The most commands one sample asks for: RB, RT, LB, LT, and with the seek
// controls offered X and the four D-pad directions.
constexpr int kMostSampled = 9;

// A button that repeats while held: once on the press, then after
// kRepeatDelay ticks and every kRepeatEvery after.
class HoldRepeat {
public:
	// `pressed`: the button went down this tick; `held`: it is down.
	bool Sample(bool held, bool pressed) {
		if (!held) return false;
		if (pressed) { count_ = 0; repeating_ = true; return true; }
		return repeating_ && ++count_ >= kRepeatDelay && (count_ - kRepeatDelay) % kRepeatEvery == 0;
	}
	void Reset() { count_ = 0; repeating_ = false; }
private:
	unsigned count_ = 0;
	// The button went down since the last Reset, so holding it repeats.
	bool repeating_ = false;
};

class PadControls {
public:
	// The game offers the seek controls (SeekView::offered); SF4 never does.
	void Offer(bool seek) { seek_ = seek; }
	// Writes what this sample asks for to `out` (room for kMostSampled, 4
	// without the seek controls); the count.
	// blocked: the game's pause menu is open, whose input the buttons are.
	// They ask for nothing then, and what is held when it closes counts as
	// already pressed, so neither a press made under the menu nor a held RT
	// acts once it is gone: each needs a fresh press.
	int Sample(std::uint32_t physical, Command* out, bool blocked = false) {
		if (blocked) { Reset(physical); return 0; }
		const std::uint32_t down = physical & ~previous_;
		previous_ = physical;
		int count = 0;
		if (down & input::xinput::RB) out[count++] = Command::TogglePause;
		if (step_.Sample((physical & input::xinput::RT) != 0, (down & input::xinput::RT) != 0)) out[count++] = Command::Step;
		if (down & input::xinput::LB) out[count++] = Command::CycleSpeed;
		if (down & input::xinput::LT) out[count++] = Command::Inputs;
		if (!seek_) return count;
		if (back_.Sample((physical & input::xinput::X) != 0, (down & input::xinput::X) != 0)) out[count++] = Command::StepBack;
		if (down & input::xinput::DPadLeft) out[count++] = Command::JumpBack;
		if (down & input::xinput::DPadRight) out[count++] = Command::JumpForward;
		if (down & input::xinput::DPadDown) out[count++] = Command::Restart;
		if (down & input::xinput::DPadUp) out[count++] = Command::Loop;
		return count;
	}
	// The buttons held now count as already pressed, and a held RT or X does
	// not repeat until it is pressed again.
	void Reset(std::uint32_t held = 0) { previous_ = held; step_.Reset(); back_.Reset(); }
private:
	std::uint32_t previous_ = 0;
	HoldRepeat step_, back_;
	bool seek_ = false;
};

// The keyboard's controls during playback: F1 pause or play, F2 step, F3
// slower, F4 faster, F5 the frame meter, F9 the input lanes. Virtual key codes.
constexpr unsigned kKeyF1 = 0x70, kKeyF2 = 0x71, kKeyF3 = 0x72, kKeyF4 = 0x73, kKeyF5 = 0x74, kKeyF9 = 0x78;
inline bool PlaybackKey(unsigned key) { return (key >= kKeyF1 && key <= kKeyF5) || key == kKeyF9; }
// A game that seeks adds: the comma steps back (held, it repeats), Left and
// Right jump five seconds, Home restarts and L cycles the loop.
constexpr unsigned kKeyComma = 0xBC, kKeyLeft = 0x25, kKeyRight = 0x27, kKeyHome = 0x24, kKeyL = 0x4C;
inline bool SeekKey(unsigned key) {
	return key == kKeyComma || key == kKeyLeft || key == kKeyRight || key == kKeyHome || key == kKeyL;
}
// Whether the window procedure keeps a key message from the game: the
// playback keys as plain key presses and releases while a replay plays, and,
// for a game that seeks, the seek keys.
// With Alt they are system keys and reach the game, so Alt+F4 still closes it.
constexpr unsigned kKeyDown = 0x100, kKeyUp = 0x101; // WM_KEYDOWN, WM_KEYUP
inline bool KeepsKey(bool playback, unsigned message, unsigned key, bool seek = false) {
	return playback && (message == kKeyDown || message == kKeyUp) && (PlaybackKey(key) || (seek && SeekKey(key)));
}

// What the game thread publishes for the overlay each tick.
struct View {
	// The playback this view is of: it changes each time a battle starts or
	// closes. A command names the playback it was made under and is taken
	// only for that one (sf4e__ReplayPlayback.hxx: Submit).
	std::uint32_t session = 0;
	bool playback = false;   // Playback(), in a battle
	bool armed = false;      // the transport acts now (Route::Transport)
	bool exporting = false;
	bool paused = false;
	int divisor = 1;
	// The round, cursor and rounds' frames as the last battle update left
	// them (Observation); roundFrames are the recorder's streams' frames
	// (ReplayRecorder.hxx: Stream::frames), 0 past its last round.
	int round = 0;
	std::uint32_t cursor = 0;
	std::uint32_t roundFrames[kRounds] = {};
	bool meter = false;      // the frame meter is shown (the request, or F5)
	bool lanes = false;      // the input lanes are asked for (F9, LT)
	Device device = Device::Keyboard;  // where the last command came from
	// Counts that only go up: commands taken, and transport commands
	// refused outside the fight. The overlay times the strip and the
	// "works during the fight" chip from their changes.
	std::uint32_t taken = 0, refused = 0;
	// What a game that seeks adds (SeekView); not offered by SF4.
	SeekView seek;
};

// The strip's opacity. Paused or slower it stays; at 1x it shows for
// kStripSeconds after the last command, then fades over kStripFade.
// `since` is negative before any command.
constexpr double kStripSeconds = 3, kStripFade = .5, kChipSeconds = 2;
inline float StripAlpha(bool paused, int divisor, double since) {
	if (paused || divisor != 1) return 1;
	if (since < 0 || since >= kStripSeconds + kStripFade) return 0;
	return since <= kStripSeconds ? 1.f : static_cast<float>(1 - (since - kStripSeconds) / kStripFade);
}

// A cursor as a time into the round at 60 frames a second, "0:41.17".
// Digits only, so it needs no translation.
inline void FormatClock(std::uint32_t frames, char (&out)[16]) {
	const std::uint32_t seconds = frames / 60, hundredths = frames % 60 * 100 / 60;
	const std::uint32_t minutes = seconds / 60 % 1000;
	int at = 0;
	char digits[4]; int count = 0;
	std::uint32_t m = minutes;
	do { digits[count++] = static_cast<char>('0' + m % 10); m /= 10; } while (m && count < 3);
	while (count) out[at++] = digits[--count];
	out[at++] = ':';
	out[at++] = static_cast<char>('0' + seconds % 60 / 10); out[at++] = static_cast<char>('0' + seconds % 10);
	out[at++] = '.';
	out[at++] = static_cast<char>('0' + hundredths / 10); out[at++] = static_cast<char>('0' + hundredths % 10);
	out[at] = 0;
}

} }
