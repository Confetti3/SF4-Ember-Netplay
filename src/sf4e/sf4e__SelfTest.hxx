#pragma once

#include <string>

#include "../training/FrameMeter.hxx"

// The in-game self-test (docs/development/IN_GAME_SELFTEST.md), for what a
// unit test cannot reach. SF4E_SELFTEST names the test; every step's outcome
// goes to the log and to the file SF4E_SELFTEST_RESULT names, and the game
// closes itself at the end. Without the variable nothing here runs.
namespace sf4e { namespace ui { class MenuNavigation; } }
namespace sf4e { namespace selftest {

enum class Test { None, Training, RoomHost, RoomGuest, RoomLayout, Replay, TournamentHost, TournamentGuest, Menu };

// The test a name names; None for a name no test has. scripts/run-selftest.ps1
// checks its -Test against the same names.
inline Test Parse(const std::wstring& name) {
	if (name == L"training") return Test::Training;
	if (name == L"room-host") return Test::RoomHost;
	if (name == L"room-guest") return Test::RoomGuest;
	if (name == L"room-layout") return Test::RoomLayout;
	if (name == L"replay") return Test::Replay;
	if (name == L"tournament-host") return Test::TournamentHost;
	if (name == L"tournament-guest") return Test::TournamentGuest;
	if (name == L"menu") return Test::Menu;
	return Test::None;
}

#ifdef SF4E_SELFTEST
// The test SF4E_SELFTEST names, read once. A name no test has is logged.
// Any thread.
Test Requested();

// Game thread, once a runtime tick. screen: the native screen in front, empty
// while it changes.
void Tick(bool atMainMenu, const std::string& screen);
// Game thread, each time the runtime reads the controller for a room's match
// this PC fights in, after the read: a tournament test plays `side` itself,
// and replaces the input it was given. The read is also made on a frame GGPO
// then stalls and repeats, so a test counts reads, not frames.
void MatchInput(int side, unsigned& mapped, unsigned& raw);
// Overlay thread, each frame before the menu reads its input: the menu test
// adds the buttons its walk presses. True while the walk wants the menu shown.
bool MenuFrame(const ui::MenuNavigation& nav, unsigned& buttons);
#else
// A build without the self-test (CMake option SF4E_SELFTEST) runs none.
inline Test Requested() { return Test::None; }
inline void Tick(bool, const std::string&) {}
inline void MatchInput(int, unsigned&, unsigned&) {}
inline bool MenuFrame(const ui::MenuNavigation&, unsigned&) { return false; }
#endif

// A test runs: nobody sits in front of the game, so it runs on behind other
// windows as a watched match does. Any thread.
inline bool Running() { return Requested() != Test::None; }

// For each test's own steps, in a file of its own: Tick reaches the main menu,
// then hands every tick to the requested test until the test calls Finish.
// Ticks are the runtime's, sixty a second.
constexpr int kSecond = 60;
// An environment variable, empty when it is not set.
std::wstring Environment(const wchar_t* name);
// A step's outcome, to the log and the result file.
void Say(bool pass, const std::string& step, const std::string& detail = {});
// The run is over: the outcome is written and the game asked to close.
void Finish();
// A test's stage: the ticks spent in it, and the frames its condition has
// held in a row. Every test's Stage ends with Done, where the outcome is written.
template <class Stage> struct Steps {
	Stage stage{};
	int waited = 0, held = 0;
	void Enter(Stage next) { stage = next; waited = 0; held = 0; }
	bool Late(int seconds) const { return waited > seconds * kSecond; }
	// The first tick of the stage and every `seconds` after it: a waiting step
	// sends again, since the runtime drops a command whose generation moved on.
	bool Every(int seconds) const { return waited % (seconds * kSecond) == 1; }
	// A failed step goes on at `otherwise`, or ends the run at Done.
	void Fail(const std::string& step, const std::string& detail, Stage otherwise = Stage::Done) {
		Say(false, step, detail);
		Enter(otherwise);
		if (otherwise == Stage::Done) Finish();
	}
};

// The training test (sf4e__SelfTest__Training.cxx). Start, at the main menu,
// is false when the test could not begin; Tick then runs every tick.
bool StartTraining();
void TickTraining();

// The room test (sf4e__SelfTest__Room.cxx), both of its sides.
bool StartRoom();
void TickRoom();

// The room layout test (sf4e__SelfTest__RoomLayout.cxx), on one PC.
bool StartRoomLayout();
void TickRoomLayout();

// The replay test (sf4e__SelfTest__Replay.cxx). It starts once the main menu
// is in front and ReplayReady, the game's replay table is.
bool ReplayReady();
bool StartReplay();
void TickReplay(bool atMainMenu);

// The tournament test (sf4e__SelfTest__Tournament.cxx), both of its sides.
bool StartTournament();
void TickTournament();

// The menu test (sf4e__SelfTest__Menu.cxx).
bool StartMenu();
void TickMenu(bool atMainMenu);

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
