// What Lower input lag decides on the game thread (common/InputLagControl.hxx):
// the edit to the battle's input store, put in place and back as often as
// asked; when each battle decides it and what a failed write leaves; and
// which of the game's two keyboard passes runs in a frame.
#include "../common/InputLagControl.hxx"
#include "../common/InputLagEdit.hxx"
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "test_support.hxx"

using namespace sf4e;

namespace {
// The game's bytes around 0x5CFBB6: `mov ecx, [edi]`, then `mov edx, [esi+2Ch]`, then `push -1`.
const std::uint8_t kNative[7] = { 0x8B, 0x0F, 0x8B, 0x56, 0x2C, 0x6A, 0xFF };
const std::uint8_t kLowerLag[7] = { 0x8B, 0x0F, 0x8B, 0x56, 0x28, 0x6A, 0xFF };

struct Code {
	std::uint8_t bytes[7];
	explicit Code(const std::uint8_t (&from)[7]) { std::memcpy(bytes, from, 7); }
	code_edit::Edit Store() { return input_lag::StoreAtReadFrame(bytes + 2); }
	bool Is(const std::uint8_t (&want)[7]) const { return std::memcmp(bytes, want, 7) == 0; }
};

// Refuses every unlock while `refuse` is set; counts writes and what stays writable.
struct Pages {
	bool refuse = false;
	int unlocks = 0, open = 0;
	bool Unlock(std::uint8_t*, std::uint8_t, unsigned long& saved) {
		if (refuse) return false;
		++unlocks; ++open;
		saved = 0x20; // PAGE_EXECUTE_READ
		return true;
	}
	void Lock(std::uint8_t*, std::uint8_t, unsigned long saved) {
		CHECK(saved == 0x20);
		--open;
	}
};

void TestStoreEdit() {
	Code code(kNative);
	const auto edit = code.Store();
	Pages pages;
	// On, on again (no write), off, off again (no write).
	CHECK(code_edit::Set(edit, true, pages) && code.Is(kLowerLag) && pages.unlocks == 1);
	CHECK(code_edit::Set(edit, true, pages) && pages.unlocks == 1);
	CHECK(code_edit::Set(edit, false, pages) && code.Is(kNative) && pages.unlocks == 2);
	CHECK(code_edit::Set(edit, false, pages) && pages.unlocks == 2 && pages.open == 0);
	// Already in the state asked for, nothing needs to be writable.
	Pages refusing; refusing.refuse = true;
	CHECK(code_edit::Set(edit, false, refusing));
	// A place that cannot be made writable stays as it was.
	CHECK(!code_edit::Set(edit, true, refusing) && code.Is(kNative));
	// Bytes of another game build are left alone either way.
	Code other(kNative);
	other.bytes[4] = 0x30;
	Pages untouched;
	CHECK(!code_edit::Set(other.Store(), true, untouched) && !code_edit::Set(other.Store(), false, untouched));
	CHECK(untouched.unlocks == 0 && other.bytes[4] == 0x30);
}

// An offline battle takes the setting at its first frame and keeps it: a
// change mid-battle waits for the next battle. Closing puts the game's
// bytes back, and the next battle decides again.
void TestOfflineBattles() {
	Code code(kNative);
	Pages pages;
	input_lag::Battle battle;
	CHECK(battle.BeforeFrame(code.Store(), true, pages) && code.Is(kLowerLag));
	CHECK(!battle.BeforeFrame(code.Store(), false, pages) && code.Is(kLowerLag));
	CHECK(!battle.BeforeFrame(code.Store(), true, pages) && pages.unlocks == 1);
	CHECK(battle.Close(code.Store(), pages) && code.Is(kNative));
	CHECK(battle.BeforeFrame(code.Store(), false, pages) && code.Is(kNative) && pages.unlocks == 2);
	CHECK(!battle.BeforeFrame(code.Store(), true, pages) && code.Is(kNative));
	CHECK(battle.Close(code.Store(), pages) && code.Is(kNative) && pages.unlocks == 2 && pages.open == 0);
	CHECK(!battle.Unavailable());
}

// A room's match sets the store before GGPO starts and the battle's frames
// leave it there, whatever the setting says.
void TestMatches() {
	Code code(kNative);
	Pages pages;
	input_lag::Battle battle;
	CHECK(battle.StartMatch(code.Store(), true, pages) && code.Is(kLowerLag));
	CHECK(!battle.BeforeFrame(code.Store(), false, pages) && code.Is(kLowerLag));
	CHECK(battle.Close(code.Store(), pages) && code.Is(kNative));
	CHECK(battle.StartMatch(code.Store(), false, pages) && code.Is(kNative));
	CHECK(!battle.BeforeFrame(code.Store(), true, pages) && code.Is(kNative));
	CHECK(battle.Close(code.Store(), pages));
	// The issue's own mod patched the game's file: the bytes read the edited
	// state, which is still one Ember knows, so either state can be set.
	Code modded(kLowerLag);
	input_lag::Battle onModded;
	CHECK(onModded.StartMatch(modded.Store(), false, pages) && modded.Is(kNative));
	CHECK(onModded.Close(modded.Store(), pages) && !onModded.Unavailable());
}

// A write that fails makes the edit unavailable for the run: nothing more is
// written, and a match starts only if the store already reads what it needs.
void TestFailedWrites() {
	{
		// Setting it for a match fails: that match does not start; a match
		// that needs the game's own bytes still does.
		Code code(kNative);
		Pages refusing; refusing.refuse = true;
		input_lag::Battle battle;
		CHECK(!battle.StartMatch(code.Store(), true, refusing) && battle.Unavailable() && code.Is(kNative));
		CHECK(battle.Close(code.Store(), refusing));
		Pages pages;
		CHECK(!battle.StartMatch(code.Store(), true, pages) && pages.unlocks == 0);
		CHECK(battle.Close(code.Store(), pages));
		CHECK(battle.StartMatch(code.Store(), false, pages) && code.Is(kNative));
		CHECK(battle.Close(code.Store(), pages));
		// An offline battle is decided anyway and plays as the store reads.
		CHECK(battle.BeforeFrame(code.Store(), true, pages) && code.Is(kNative) && pages.unlocks == 0);
	}
	{
		// Putting it back fails: the edit stays, so a match that needs the
		// game's own bytes is refused from then on; one that needs the edit
		// may start.
		Code code(kNative);
		Pages pages;
		input_lag::Battle battle;
		CHECK(battle.BeforeFrame(code.Store(), true, pages) && code.Is(kLowerLag));
		Pages refusing; refusing.refuse = true;
		CHECK(!battle.Close(code.Store(), refusing) && battle.Unavailable() && code.Is(kLowerLag));
		CHECK(!battle.StartMatch(code.Store(), false, pages) && code.Is(kLowerLag) && pages.unlocks == 1);
		CHECK(!battle.Close(code.Store(), pages));
		CHECK(battle.StartMatch(code.Store(), true, pages) && pages.unlocks == 1);
	}
	{
		// Bytes of neither state: no match starts, with or without the edit.
		Code code(kNative);
		code.bytes[4] = 0x30;
		Pages pages;
		input_lag::Battle battle;
		CHECK(!battle.StartMatch(code.Store(), false, pages) && battle.Unavailable());
		CHECK(!battle.Close(code.Store(), pages));
		CHECK(!battle.StartMatch(code.Store(), true, pages) && pages.unlocks == 0 && code.bytes[4] == 0x30);
	}
}

// One frame as the game runs it: the pad update first, then the device loop.
// Returns how many passes turned key input into key states, and whether
// the pad update saw this frame's keys.
struct Frame { int passes = 0; bool beforePadUpdate = false; };
Frame RunFrame(input_lag::Keyboard& keyboard, bool setting, bool hooked) {
	Frame frame;
	if (keyboard.BeforePadUpdate(setting, hooked)) { ++frame.passes; frame.beforePadUpdate = true; }
	// The pad update reads the key states here.
	if (keyboard.DeviceLoopPass()) ++frame.passes;
	return frame;
}

// One pass a frame whatever the row does: before the pad update with it on,
// in the device loop with it off, or while the hook is not in place.
void TestKeyboardPasses() {
	input_lag::Keyboard keyboard;
	CHECK(keyboard.DeviceLoopPass());
	const struct { bool setting, hooked, early; } frames[] = {
		{false, true, false}, {true, true, true}, {true, true, true}, {false, true, false},
		{true, false, false}, {true, true, true}, {false, false, false}, {true, true, true},
	};
	for (const auto& expected : frames) {
		const auto frame = RunFrame(keyboard, expected.setting, expected.hooked);
		CHECK(frame.passes == 1 && frame.beforePadUpdate == expected.early && keyboard.First() == expected.early);
	}
}
}

int main() {
	TestStoreEdit();
	TestOfflineBattles();
	TestMatches();
	TestFailedWrites();
	TestKeyboardPasses();
	std::puts("InputLagControl test passed");
	return 0;
}
