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

// Where a replay takeover stands (ReplayTakeover.hxx has its rules).
//   Off: watching the replay.
//   Choosing: the takeover timeline is open over the paused replay.
//   Countdown: the player's side is taken; the battle holds 3 s.
//   Playing: the player plays the side.
enum class Stage : std::uint8_t { Off, Choosing, Countdown, Playing };
inline bool Taken(Stage stage) { return stage == Stage::Countdown || stage == Stage::Playing; }

// What the player can ask for. The first five act on the transport and
// only in the fight; Meter and Inputs show or hide a layer at any point of
// the playback. The rest are the takeover's, in the fight: TakeOver opens
// its timeline, whose jumps are StepBack and a second either way (a frame
// ahead is Step), Side picks the other fighter, Start begins, Watch closes
// it; Restart and Timeline go back to the takeover's frame.
enum class Command : std::uint8_t {
	TogglePause, Step, Slower, Faster, CycleSpeed, Meter, Inputs,
	TakeOver, StepBack, SecondBack, SecondAhead, Side, Start, Watch, Restart, Timeline,
};
inline bool MovesTransport(Command command) { return command <= Command::CycleSpeed; }
// Which commands a takeover stage takes; every other one is dropped. During
// a takeover the replay neither pauses nor changes speed, and shows neither
// layer; the timeline stays paused, a Step its frame ahead.
inline bool Accepts(Stage stage, Command command) {
	switch (stage) {
	case Stage::Off: return command <= Command::TakeOver;
	case Stage::Choosing: return command == Command::Step || (command >= Command::StepBack && command <= Command::Watch);
	case Stage::Countdown: case Stage::Playing: return command == Command::Restart || command == Command::Timeline;
	}
	return false;
}
// A second of the fight, the timeline's longer jump.
constexpr int kSecondFrames = 60;
// The frames a timeline jump moves; 0 for any other command.
inline int JumpFrames(Command command) {
	return command == Command::StepBack ? -1 : command == Command::SecondBack ? -kSecondFrames : command == Command::SecondAhead ? kSecondFrames : 0;
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
	bool fight = false;       // IsFight (0x5D9F60)
	bool pauseMenu = false;   // kPauseOrHold is set
	bool taken = false;       // a takeover has the player's side (Taken)
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
//   Takeover: the player plays a side. Ember's cadence at 1x, held only by
//             the countdown: no pause, no half speed, no jump, no snapshot.
// Ember's cadences only run in the fight with the pause menu closed. The
// game's own drives the knockout, the round's end and the pause menu, after
// Ember hands it back (Relinquish), so all of those play at 1x.
enum class Route { Original, FullSpeed, Transport, Takeover };
inline Route Decide(const Context& context) {
	if (!Playback(context)) return Route::Original;
	if (context.exporting) return Route::FullSpeed;
	if (!context.fight || context.pauseMenu) return Route::Original;
	return context.taken ? Route::Takeover : Route::Transport;
}

// A new round is being set up: the transport starts it playing at 1x, so a
// pause left on at the end of a round cannot freeze the next one on its
// first frame of the fight.
inline bool RoundBoundary(int flow) { return flow == kFlowRoundStart || flow == kFlowReady; }

// Whether a jump along the replay (training/CameraControl.hxx: Seek) has
// reached its frame: `at` in round `round` is the goal's or past it. A jump
// back first waits for its snapshot to load (before the next battle update),
// and until then the battle still stands where the jump left from, past the
// goal, which is not the goal reached.
inline bool JumpLanded(bool loadWaiting, int round, int at, int goalRound, int goalFrame) {
	return !loadWaiting && (round > goalRound || (round == goalRound && at >= goalFrame));
}
// Whether a takeover may begin at the frame shown: no jump is asked
// (`seekAsked`, not yet taken by the cadence), playing to its frame
// (`seeking`) or waiting for its snapshot to load (`loadWaiting`). A jump
// left in any of those would move the battle after the takeover's frame
// was saved, and its snapshot would hand the side back to the replay.
inline bool TakeoverMayBegin(bool seekAsked, bool seeking, bool loadWaiting) { return !seekAsked && !seeking && !loadWaiting; }

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

// What the pad holds in a game tick: its physical XInput buttons, and
// whether it holds left or right (the stick's too, as the game reads them).
struct PadState { std::uint32_t buttons = 0; bool left = false, right = false; };

// The pad's controls, sampled once a game tick, by the takeover's stage:
//   Off: RB pause or play, RT step (held, again after kRepeatDelay ticks and
//     every kRepeatEvery after), LB the next speed, LT the input lanes, Y the
//     takeover timeline. Start and View stay the game's.
//   Choosing: left and right a frame back or ahead (held, repeating as RT
//     does), LB and RB a second back or ahead, Y the other fighter, A starts,
//     B watches on from the frame.
//   Countdown and Playing: every other button is the fighter's. View
//     restarts on its release, or opens the timeline once held kViewHold ticks.
constexpr unsigned kRepeatDelay = 18, kRepeatEvery = 6; // 300 ms, then 10 a second
constexpr unsigned kViewHold = 30;                      // 500 ms
class PadControls {
public:
	// Writes what this sample asks for to `out` (room for 4); the count.
	// blocked: the game's pause menu is open, whose input the buttons are.
	// They ask for nothing then, and what is held when it closes counts as
	// already pressed, so neither a press made under the menu nor a held RT
	// acts once it is gone: each needs a fresh press. A change of stage
	// counts what is held as already pressed too.
	int Sample(PadState pad, Stage stage, bool blocked, Command* out) {
		if (blocked || stage != stage_) { Reset(pad); stage_ = stage; return 0; }
		const std::uint32_t down = pad.buttons & ~previous_.buttons;
		const bool leftDown = pad.left && !previous_.left, rightDown = pad.right && !previous_.right;
		previous_ = pad;
		int count = 0;
		switch (stage) {
		case Stage::Off:
			if (down & input::xinput::RB) out[count++] = Command::TogglePause;
			if (Repeats(Held::RT, (pad.buttons & input::xinput::RT) != 0, (down & input::xinput::RT) != 0)) out[count++] = Command::Step;
			if (down & input::xinput::LB) out[count++] = Command::CycleSpeed;
			if (down & input::xinput::LT) out[count++] = Command::Inputs;
			if (down & input::xinput::Y) out[count++] = Command::TakeOver;
			break;
		case Stage::Choosing:
			if (Repeats(Held::Left, pad.left, leftDown)) out[count++] = Command::StepBack;
			else if (Repeats(Held::Right, pad.right, rightDown)) out[count++] = Command::Step;
			if (down & input::xinput::LB) out[count++] = Command::SecondBack;
			if (down & input::xinput::RB) out[count++] = Command::SecondAhead;
			if (down & input::xinput::Y) out[count++] = Command::Side;
			if (down & input::xinput::A) out[count++] = Command::Start;
			else if (down & input::xinput::B) out[count++] = Command::Watch;
			break;
		case Stage::Countdown: case Stage::Playing: {
			const bool view = (pad.buttons & input::xinput::View) != 0;
			if (down & input::xinput::View) { viewHeld_ = 0; viewDown_ = true; }
			else if (viewDown_ && view && ++viewHeld_ == kViewHold) { viewDown_ = false; out[count++] = Command::Timeline; }
			else if (viewDown_ && !view) { viewDown_ = false; out[count++] = Command::Restart; }
			break;
		}
		}
		return count;
	}
	// What is held now counts as already pressed, and nothing held repeats
	// until it is pressed again.
	void Reset(PadState held = {}) { previous_ = held; held_ = 0; repeating_ = Held::None; viewHeld_ = 0; viewDown_ = false; }
private:
	enum class Held : std::uint8_t { None, RT, Left, Right };
	// A press, then again while it stays held, as RT steps.
	bool Repeats(Held which, bool held, bool down) {
		if (!held) { if (repeating_ == which) repeating_ = Held::None; return false; }
		if (down) { held_ = 0; repeating_ = which; return true; }
		return repeating_ == which && ++held_ >= kRepeatDelay && (held_ - kRepeatDelay) % kRepeatEvery == 0;
	}
	PadState previous_;
	unsigned held_ = 0, viewHeld_ = 0;
	// What went down since the last Reset, so holding it repeats.
	Held repeating_ = Held::None;
	// View went down during a takeover and has not acted yet.
	bool viewDown_ = false;
	Stage stage_ = Stage::Off;
};

// The keyboard's controls during playback: F1 pause or play, F2 step, F3
// slower, F4 faster, F5 the frame meter, F9 the input lanes, F6 the takeover
// timeline (Restart during a takeover), F7 its Timeline. On the takeover
// timeline also the arrows, comma and period, Tab, Enter and Escape
// (ui/ReplayPlaybackHud.cxx: ReplayKeys). Virtual key codes.
constexpr unsigned kKeyF1 = 0x70, kKeyF2 = 0x71, kKeyF3 = 0x72, kKeyF4 = 0x73, kKeyF5 = 0x74, kKeyF6 = 0x75, kKeyF7 = 0x76, kKeyF9 = 0x78;
constexpr unsigned kKeyTab = 0x09, kKeyEnter = 0x0D, kKeyEscape = 0x1B, kKeyLeft = 0x25, kKeyRight = 0x27, kKeyComma = 0xBC, kKeyPeriod = 0xBE;
inline bool PlaybackKey(unsigned key) { return (key >= kKeyF1 && key <= kKeyF7) || key == kKeyF9; }
inline bool TimelineKey(unsigned key) {
	return key == kKeyTab || key == kKeyEnter || key == kKeyEscape || key == kKeyLeft || key == kKeyRight || key == kKeyComma || key == kKeyPeriod;
}
// Who the playback keys are, for the window procedure: the game's outside a
// replay Ember's controls play (`playback`), the timeline's while it is open.
enum class Keys : std::uint8_t { Game, Playback, Timeline };
inline Keys KeysFor(bool playback, Stage stage) {
	return !playback ? Keys::Game : stage == Stage::Choosing ? Keys::Timeline : Keys::Playback;
}
// Whether the window procedure keeps a key message from the game: the
// playback keys as plain key presses and releases while a replay plays, and
// the timeline's while it is open, Escape among them, so it never reaches the
// game's pause menu. With Alt they are system keys and reach the game, so
// Alt+F4 still closes it.
constexpr unsigned kKeyDown = 0x100, kKeyUp = 0x101; // WM_KEYDOWN, WM_KEYUP
inline bool KeepsKey(Keys keys, unsigned message, unsigned key) {
	if (keys == Keys::Game || (message != kKeyDown && message != kKeyUp)) return false;
	return PlaybackKey(key) || (keys == Keys::Timeline && TimelineKey(key));
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
	// The takeover (ReplayTakeover.hxx): its stage, the side, the countdown
	// ticks left, the battle frame it last began at (-1: none this round; the
	// timeline counts in these, CameraControl.hxx: Frame), and that the
	// recording of the round ran out while the player plays.
	Stage takeover = Stage::Off;
	int takeoverSide = 0, countdown = 0, takeoverFrame = -1;
	bool recordingEnded = false;
	// A takeover could not start or go back: its save state failed.
	std::uint32_t takeoverFailed = 0;
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
