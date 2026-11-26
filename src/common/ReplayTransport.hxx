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
// ReplayTransportTest covers every rule here.

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

#include "PadKind.hxx"
#include "ReplayTakeover.hxx"
#include "TrainingPad.hxx"

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

// What the player can ask for.
enum class Command : std::uint8_t { TogglePause, Step, Slower, Faster, CycleSpeed, Meter, Inputs, TakeoverPrimary, TakeoverSecondary, TakeoverOther };
// What a command acts on: the transport, only in the fight; a layer shown or
// hidden (the meter, the input lanes) at any point of the playback; or replay
// takeover (sf4e__ReplayTakeover), in the fight and while the player has a fighter.
enum class Kind { Transport, Layer, Takeover };
inline Kind KindOf(Command command) {
	switch (command) {
	case Command::Meter: case Command::Inputs: return Kind::Layer;
	case Command::TakeoverPrimary: case Command::TakeoverSecondary: case Command::TakeoverOther: return Kind::Takeover;
	default: return Kind::Transport;
	}
}
// What a takeover command does in each phase: Primary takes Player 1, or
// retries a takeover; Secondary takes Player 2, or goes back to the replay;
// Other changes what the other fighter does. Taken in the phase it acts in.
inline takeover::Step StepOf(Command command, takeover::Phase phase) {
	using takeover::Step;
	if (command == Command::TakeoverOther) return Step::CycleOther;
	const bool primary = command == Command::TakeoverPrimary;
	if (phase == takeover::Phase::Off) return primary ? Step::TakePlayer1 : Step::TakePlayer2;
	return primary ? Step::Retry : Step::BackToReplay;
}
// Where a command came from, for the glyphs the strip shows.
enum class Device : std::uint8_t { Keyboard, Pad };

// What the cadence call sees. Read only what is needed: nothing but
// `netplay` and `recorderMode` is read outside a replay's playback.
struct Context {
	bool netplay = false;     // a GGPO session exists
	int recorderMode = 0;     // ReplaySystem +0x708
	bool training = false;    // the battle is Training
	bool exporting = false;   // the playback is being made into a video
	// A takeover is on (ReplayTakeover.hxx: Phase): the recorder is stopped while
	// the player has a fighter, and the battle is held once its round is over.
	takeover::Phase takeover = takeover::Phase::Off;
	bool fight = false;       // IsFight (0x5D9F60)
	bool pauseMenu = false;   // kPauseOrHold is set
};

// A replay is playing back: the game reports it and nothing else owns the
// battle. Netplay, Versus and Training never are.
inline bool Playback(const Context& context) {
	return !context.netplay && (context.recorderMode == kRecorderPlaying || context.takeover != takeover::Phase::Off) && !context.training;
}

// What the cadence call does.
//   Original: the game's own function, unchanged.
//   FullSpeed: an export. The game's function with its half speed cleared
//              first, so the video always plays at 1x.
//   Transport: Ember's cadence; the controls act.
// Ember's cadence only runs in the fight with the pause menu closed. The
// game's own drives the knockout, the round's end and the pause menu, after
// Ember hands it back (Relinquish), so all of those play at 1x.
//   Takeover: a takeover's cadence, always 1x, held once its round is over;
//             the transport's own controls stand down.
enum class Route { Original, FullSpeed, Transport, Takeover };
inline Route Decide(const Context& context) {
	if (!Playback(context)) return Route::Original;
	if (context.exporting) return Route::FullSpeed;
	if (context.takeover != takeover::Phase::Off) return Route::Takeover;
	if (!context.fight || context.pauseMenu) return Route::Original;
	return Route::Transport;
}

// What the cadence call does with a command: acts on it, refuses it (a
// control asked for outside the fight, which the strip's chip answers), or
// drops it unseen: an export plays untouched, under the pause menu the keys
// are its own, and while the player has a fighter the transport stands down.
enum class Fate { Take, Refuse, Drop };
inline Fate FateOf(Command command, Route route, const Context& context) {
	if (context.exporting || context.pauseMenu) return Fate::Drop;
	switch (KindOf(command)) {
	case Kind::Layer: return Fate::Take;
	case Kind::Takeover: return route == Route::Transport || route == Route::Takeover ? Fate::Take : Fate::Refuse;
	case Kind::Transport: break;
	}
	return route == Route::Transport ? Fate::Take : route == Route::Takeover ? Fate::Drop : Fate::Refuse;
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

// The pad's controls while a replay plays, from its physical XInput buttons
// sampled once a game tick: RB pause or play, RT step (held, again after
// kRepeatDelay ticks and every kRepeatEvery after), LB the next speed, LT the
// input lanes; L3 takes Player 1 over, R3 Player 2, and Up changes what the
// other fighter would do (directions do nothing in a replay). Start and Back
// stay the game's.
constexpr unsigned kRepeatDelay = 18, kRepeatEvery = 6; // 300 ms, then 10 a second
constexpr int kMostPadCommands = 7;
class PadControls {
public:
	// Writes what this sample asks for to `out` (room for kMostPadCommands); the count.
	// blocked: the game's pause menu is open, whose input the buttons are, or
	// the player has a fighter (TakeoverPad). They ask for nothing then, and
	// what is held when that ends counts as already pressed, so neither a
	// press made meanwhile nor a held RT acts once it is gone: each needs a fresh press.
	int Sample(std::uint32_t physical, Command* out, bool blocked = false) {
		if (blocked) { Reset(physical); return 0; }
		namespace xinput = input::xinput;
		const std::uint32_t down = physical & ~previous_;
		previous_ = physical;
		int count = 0;
		if (down & xinput::RB) out[count++] = Command::TogglePause;
		if (physical & xinput::RT) {
			if (down & xinput::RT) { held_ = 0; repeating_ = true; out[count++] = Command::Step; }
			else if (repeating_ && ++held_ >= kRepeatDelay && (held_ - kRepeatDelay) % kRepeatEvery == 0) out[count++] = Command::Step;
		}
		if (down & xinput::LB) out[count++] = Command::CycleSpeed;
		if (down & xinput::LT) out[count++] = Command::Inputs;
		if (down & xinput::LS) out[count++] = Command::TakeoverPrimary;
		if (down & xinput::RS) out[count++] = Command::TakeoverSecondary;
		if (down & input::PhysicalUp) out[count++] = Command::TakeoverOther;
		return count;
	}
	// The buttons held now count as already pressed, and a held RT does not
	// repeat until it is pressed again.
	void Reset(std::uint32_t held = 0) { previous_ = held; held_ = 0; repeating_ = false; }
private:
	std::uint32_t previous_ = 0;
	unsigned held_ = 0;
	// RT went down since the last Reset, so holding it steps again.
	bool repeating_ = false;
};

// The pad while the player has a fighter, whose buttons they all are but
// Back: a tap retries and a hold goes back to the replay, timed as Training's
// Back resets and saves (TrainingPadGesture). `now` is seconds.
class TakeoverPad {
public:
	// Writes what this sample asks for to `out` (room for 1); the count.
	// blocked: not taking over, or under the game's pause menu; a Back held
	// then counts only once it is pressed again.
	int Sample(std::uint32_t physical, double now, bool blocked, Command* out) {
		if (blocked) { gesture_.Reset(); return 0; }
		const input::TrainingPadEvents events = gesture_.Update(physical, false, now);
		if (events.reset) { out[0] = Command::TakeoverPrimary; return 1; }
		if (events.save) { out[0] = Command::TakeoverSecondary; return 1; }
		return 0;
	}
private:
	input::TrainingPadGesture gesture_;
};

// The keyboard's controls during playback: F1 pause or play, F2 step, F3
// slower, F4 faster, F5 the frame meter, F9 the input lanes. Virtual key codes.
constexpr unsigned kKeyF1 = 0x70, kKeyF2 = 0x71, kKeyF3 = 0x72, kKeyF4 = 0x73, kKeyF5 = 0x74, kKeyF6 = 0x75, kKeyF7 = 0x76, kKeyF8 = 0x77, kKeyF9 = 0x78;
// F1 to F5 and F9 the transport's, F6 to F8 takeover's.
inline bool PlaybackKey(unsigned key) { return (key >= kKeyF1 && key <= kKeyF8) || key == kKeyF9; }
// Whether the window procedure keeps a key message from the game: the
// playback keys as plain key presses and releases while a replay plays.
// With Alt they are system keys and reach the game, so Alt+F4 still closes it.
constexpr unsigned kKeyDown = 0x100, kKeyUp = 0x101; // WM_KEYDOWN, WM_KEYUP
inline bool KeepsKey(bool playback, unsigned message, unsigned key) {
	return playback && (message == kKeyDown || message == kKeyUp) && PlaybackKey(key);
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
	// The takeover as the last battle update left it.
	takeover::Phase takeover = takeover::Phase::Off;
	takeover::Plan plan;
	// The battle plays the replay file's inputs (the lanes and the meter's
	// inputs are the file's): not while a takeover plays something else.
	bool FileInputs() const { return takeover == takeover::Phase::Off; }
	// Counts that only go up: commands taken, and transport commands
	// refused outside the fight. The overlay times the strip and the
	// "works during the fight" chip from their changes.
	std::uint32_t taken = 0, refused = 0;
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
