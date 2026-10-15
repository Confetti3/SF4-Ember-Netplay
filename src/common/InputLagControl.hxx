#pragma once

// What Lower input lag and its keyboard row decide, apart from the game so it
// can be tested. sf4e__InputLag.cxx carries it out on the game thread.
#include "CodeEdit.hxx"

namespace sf4e { namespace input_lag {

// The battle's input store (InputLagEdit.hxx): set once per battle, before its
// first frame, and put back when the battle closes. `pages` writes the game's
// code (code_edit).
//
// A write that fails, or bytes that read neither state, make the edit
// unavailable for the rest of the run: nothing more is written. A room's
// match then starts only if the store already reads what the match needs, so
// a PC never simulates a match differently from its peers.
class Battle {
public:
	// A room's match, before GGPO starts: true when the store reads `on`, as
	// on every PC simulating the match. False: the match must not start.
	template <class Pages> bool StartMatch(const code_edit::Edit& store, bool on, Pages& pages) {
		decided_ = true;
		return Establish(store, on, pages);
	}
	// Before each battle frame: a battle no match decided follows the player's
	// setting from its first frame on, and later changes wait for the next
	// battle. True when this frame decided it. Only this PC simulates such a
	// battle, so a store that cannot follow the setting is only logged.
	template <class Pages> bool BeforeFrame(const code_edit::Edit& store, bool setting, Pages& pages) {
		if (decided_) return false;
		decided_ = true;
		Establish(store, setting, pages);
		return true;
	}
	// The battle closed: true when the game's own bytes are back.
	template <class Pages> bool Close(const code_edit::Edit& store, Pages& pages) {
		decided_ = false;
		return Establish(store, false, pages);
	}
	bool Unavailable() const { return unavailable_; }

private:
	template <class Pages> bool Establish(const code_edit::Edit& store, bool on, Pages& pages) {
		if (!unavailable_ && !code_edit::Set(store, on, pages)) unavailable_ = true;
		return code_edit::Reads(store, on);
	}
	bool decided_ = false;
	bool unavailable_ = false;
};

// The keyboard row picks which of the game's two passes over the keyboard
// runs in a frame. Each frame the pad update (0x512180) runs first and the
// device loop (0x6C97E0) after it, each pass turning the window's key input
// into key states. With the row on, Ember runs the pass just before the pad
// update reads them and the device loop's later pass is skipped, so there is
// still one pass a frame.
class Keyboard {
public:
	// Before the native pad update: true when Ember runs the pass now.
	// `hooked`: the device loop's pass can be skipped.
	bool BeforePadUpdate(bool setting, bool hooked) {
		first_ = setting && hooked;
		return first_;
	}
	// The device loop's pass later in the same frame: true when it runs.
	bool DeviceLoopPass() const { return !first_; }
	bool First() const { return first_; }

private:
	bool first_ = false;
};

}}
