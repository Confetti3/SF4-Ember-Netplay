#pragma once

#include <string>

#include "../training/FrameMeter.hxx"

// The in-game self-test (docs/development/IN_GAME_SELFTEST.md), for what a
// unit test cannot reach. SF4E_SELFTEST names the test; every step's outcome
// goes to the log and to the file SF4E_SELFTEST_RESULT names, and the game
// closes itself at the end. Without the variable nothing here runs.
namespace sf4e { namespace selftest {

enum class Test { None, Training };

// The test a name names; None for a name no test has. scripts/run-selftest.ps1
// checks its -Test against the same names.
inline Test Parse(const std::wstring& name) { return name == L"training" ? Test::Training : Test::None; }

// The test SF4E_SELFTEST names, read once. A name no test has is logged.
// Any thread.
Test Requested();

// A test runs: nobody sits in front of the game, so it runs on behind other
// windows as a watched match does. Any thread.
inline bool Running() { return Requested() != Test::None; }

// Game thread, once a runtime tick. screen: the native screen in front, empty
// while it changes.
void Tick(bool atMainMenu, const std::string& screen);

// The cells the meter draws for one side, by kind.
struct Cells { int startup = 0, active = 0, recovery = 0; };
inline Cells CountCells(const training::MeterView& meter, int side) {
	Cells cells;
	for (const auto& frame : meter.frames) {
		const auto kind = frame.cells[side].kind;
		cells.startup += kind == training::MeterKind::Startup;
		cells.active += kind == training::MeterKind::Active;
		cells.recovery += kind == training::MeterKind::Recovery;
	}
	return cells;
}

} }
