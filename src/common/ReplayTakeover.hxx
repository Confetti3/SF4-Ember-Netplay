#pragma once

// Replay takeover: from a paused replay the player picks a frame on the
// takeover timeline and a fighter, a 3-2-1 countdown holds the battle, and
// the player then plays that fighter from there while the other one keeps
// the replay's inputs. Restart goes back to that frame and counts down again;
// Timeline goes back to it and gives the fighter back to the replay, to pick
// another frame or watch on. The rules the game thread
// (sf4e__ReplayPlayback.cxx, sf4e__ReplayTakeover.cxx) follows;
// ReplayTakeoverTest covers every one.
//
// How it acts, read from the code and checked in the game (2026-11-25): the
// recorder plays a side only while the side's bit is set in its play mask
// (ReplayRecorder.hxx: Recorder::playingSides); CMD POST (0x59AA20) leaves a
// side whose bit is clear on its own pad slot's input. The other side reads
// the recording as before, frame for frame. Going back loads a save state
// taken at the takeover frame, before the bit was cleared, so it puts the
// replay back on its recorded course; a new round, Restart or Next round in
// the game's replay menu starts the round's playback with both bits set.

#include "ReplayTransport.hxx"

namespace sf4e { namespace replaytakeover {

using replaytransport::Command;
using replaytransport::Stage;

// 3 s at 60 ticks a second, from Start and from every Restart.
constexpr int kCountdownTicks = 180;

// What the game thread does for a step.
//   Begin: frees an earlier save state, saves one now, takes the side.
//   Restart: loads the save state, takes the side again.
//   Return: loads the save state, gives the side back to the replay.
//   End: gives the side back (if taken) and frees the save state.
// Begin, Restart and Return wait for the game thread (Pending), which says
// how they went (Carried); the stage changes only once they did. End is
// carried out at once and never fails.
enum class Act : std::uint8_t { None, Begin, Restart, Return, End };

class Takeover {
public:
	Stage GetStage() const { return stage_; }
	// 0 is Player 1.
	int Side() const { return side_; }
	// Ticks of the countdown left; 0 outside it.
	int Countdown() const { return countdown_; }
	// The step waiting for the game thread.
	Act Pending() const { return pending_; }

	// Whether a command is taken now: one the stage takes
	// (replaytransport::Accepts), and none while a step waits for the game
	// thread, a timeline Step or jump included.
	bool Accepts(Command command) const { return pending_ == Act::None && replaytransport::Accepts(stage_, command); }

	// A command taken (Accepts) and the step it asks of the game thread.
	// `paused`: the replay is paused under Ember's controls; `settled`: no
	// jump along the replay is asked or under way (TakeoverMayBegin, checked
	// again by the game thread before it saves).
	Act Apply(Command command, bool paused, bool settled) {
		if (!Accepts(command)) return Act::None;
		switch (command) {
		case Command::TakeOver:
			if (paused) stage_ = Stage::Choosing;
			return Act::None;
		case Command::Side: side_ = 1 - side_; return Act::None;
		case Command::Start: return settled ? pending_ = Act::Begin : Act::None;
		case Command::Restart: return pending_ = Act::Restart;
		case Command::Timeline: return pending_ = Act::Return;
		case Command::Watch: return End();
		default: return Act::None;
		}
	}

	// How the pending step went. A Begin that failed leaves the timeline as it
	// was; a load that failed has left the battle, so the takeover is over.
	void Carried(bool ok) {
		const Act act = pending_;
		pending_ = Act::None;
		if (act == Act::None) return;
		if (!ok) { if (act != Act::Begin) { stage_ = Stage::Off; countdown_ = 0; } return; }
		if (act == Act::Return) { stage_ = Stage::Choosing; countdown_ = 0; }
		else { stage_ = Stage::Countdown; countdown_ = kCountdownTicks; }
	}

	// Once a game tick in the fight: the countdown runs down, and the battle
	// holds while it does.
	bool Hold() {
		if (stage_ != Stage::Countdown) return false;
		if (--countdown_ > 0) return true;
		countdown_ = 0; stage_ = Stage::Playing;
		return false;
	}

	// Watch, or the playback left the fight it was taken in (a round
	// boundary, the battle's close, an export starting): the step waiting
	// goes, and End is due at any stage but Off. Off holds no side and no
	// save state: every way there is an End, or a failed load, which has left
	// the battle (its close frees the state).
	Act End() {
		const bool open = stage_ != Stage::Off;
		stage_ = Stage::Off; countdown_ = 0; pending_ = Act::None;
		return open ? Act::End : Act::None;
	}

private:
	Stage stage_ = Stage::Off;
	Act pending_ = Act::None;
	int side_ = 0, countdown_ = 0;
};

// Countdown ticks as the whole seconds shown, 3, 2, 1.
inline int CountdownSeconds(int ticks) { return ticks <= 0 ? 0 : (ticks + 59) / 60; }

} }
