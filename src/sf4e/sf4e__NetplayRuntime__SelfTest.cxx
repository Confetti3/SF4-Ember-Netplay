#include "sf4e__NetplayRuntime.hxx"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../platform/Utf8.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/FrameMeter.hxx"
#include "../training/TrainingRuntime.hxx"
#include "sf4e__BackgroundPlay.hxx"
#include "sf4e__GameEvents.hxx"

// A test that runs in the game itself, for what a unit test cannot reach:
// the frame meter against moves whose frame data is known, typed in numpad
// notation and played by the lab, and the lab's saved position. SF4E_SELFTEST=training
// starts it: an offline Versus battle of Ryu against Ryu is entered the way a
// room's match is, with no menu, the lab is let into that battle, and every
// step's outcome goes to the log and to the file SF4E_SELFTEST_RESULT names.
// The game is closed at the end. scripts/run-selftest.ps1 starts a build this
// way and reads the result. Without the variable nothing here runs.
namespace sf4e { namespace NetplayFacade { namespace internal {
namespace {

using training::Action;
using training::Command;
using training::MeterKind;
using rMainMenu = Dimps::GameEvents::MainMenu;
using rVsMode = Dimps::GameEvents::VsMode;
using Dimps::Game::ProgressData;

// Ticks are the runtime's, sixty a second.
constexpr int kSecond = 60;
enum class Stage { Off, ToMainMenu, ToBattle, Move, Moving, Settled, SavePosition, Dash, Dashing, Reset, Closing, Closed };

// A move the lab plays and its frame data: startup, active and recovery, -1
// where the test only reports what the meter showed. The numbers on the
// meter's line have to be the frame data's. Its cells follow the startup
// number (FrameMeter.hxx: ClassifyMeter), so the recovery has one cell less.
struct Case { const char* moves; int startup, active, recovery; const char* what; };
const Case kCases[] = {
	{"2LP", 3, 2, 7, "Ryu crouching LP"},
	// At the round's starting distance this is the far one.
	{"5LP", -1, -1, -1, "Ryu far standing LP"},
	{"2MK", -1, -1, -1, "Ryu crouching MK"},
	{"5HP", -1, -1, -1, "Ryu standing HP"},
	{"236LP", -1, -1, -1, "Ryu LP Hadoken"},
};

struct Run {
	Stage stage = Stage::Off;
	bool read = false, failed = false, atTitle = false;
	int waited = 0, quiet = 0;
	std::size_t at = 0;
	float saved = 0;
	std::string resultFile;
	std::vector<std::string> lines;
} s_run;

void Say(bool pass, const std::string& step, const std::string& detail = {}) {
	const std::string line = std::string(pass ? "PASS " : "FAIL ") + step + (detail.empty() ? "" : ": " + detail);
	if (pass) spdlog::info("SelfTest: {}", line); else spdlog::error("SelfTest: {}", line);
	s_run.lines.push_back(line);
	if (!pass) s_run.failed = true;
}
void Note(const std::string& line) { spdlog::info("SelfTest: {}", line); s_run.lines.push_back("INFO " + line); }

void Enter(Stage stage) { s_run.stage = stage; s_run.waited = 0; s_run.quiet = 0; }

// The outcome is written, and the game asked to close as its updater asks it.
void Finish() {
	s_run.lines.push_back(s_run.failed ? "RESULT fail" : "RESULT pass");
	spdlog::info("SelfTest: {}", s_run.lines.back());
	if (!s_run.resultFile.empty()) {
		std::ofstream out(platform::Utf8ToWide(s_run.resultFile.c_str()), std::ios::trunc);
		for (const std::string& line : s_run.lines) out << line << '\n';
	}
	auto* const main = Dimps::Platform::Main::staticMethods.GetSingleton();
	if (main) PostMessageW((*Dimps::Platform::Main::GetWindowData(main))->hWnd, WM_CLOSE, 0, 0);
	Enter(Stage::Closing);
}

// The game's foreground event by name, once the game says it is up; the
// getter is not safe to call before that.
std::string Screen() {
	if (!runtime || !runtime->ready) return {};
	auto* const root = Dimps::App::GetRootEvent();
	auto* const controller = root ? (reinterpret_cast<Dimps::Event::EventBaseWithEC*>(root)->*Dimps::Event::EventBaseWithEC::publicMethods.GetChildEventController)() : nullptr;
	auto* const event = controller ? (controller->*Dimps::Event::EventController::publicMethods.GetForegroundEvent)() : nullptr;
	return event ? Dimps::Event::EventBase::GetName(event) : std::string();
}

bool Late(int seconds) { return ++s_run.waited > seconds * kSecond; }

// Ryu against Ryu on the first stage, as a room's match puts its fighters
// into VS mode (sf4e__UserApp.cxx).
void SetFighters() {
	char* query[] = { const_cast<char*>("VSMode") };
	rVsMode* const mode = reinterpret_cast<rVsMode*>(Dimps::Event::EventBaseWithEC::FindForegroundEvent(Dimps::App::GetRootEvent(), query, 1));
	if (!mode) { spdlog::error("SelfTest: VS mode is not in front; the fighters were not set"); return; }
	rVsMode::ConfirmedCharaConditions chara{};
	chara.charaID = 0; chara.unc_edition = 14;
	rVsMode::ConfirmedPlayerConditions* const conditions = rVsMode::GetConfirmedPlayerConditions(mode);
	for (int side = 0; side < 2; side++) {
		*(rVsMode::ConfirmedPlayerConditions::GetCharaID(&conditions[side])) = chara.charaID;
		*(rVsMode::ConfirmedPlayerConditions::GetSideActive(&conditions[side])) = 1;
		*rVsMode::ConfirmedPlayerConditions::GetCharaConditions(&conditions[side]) = chara;
	}
	(rVsMode::GetStageName(mode)->*Dimps::Platform::dString::publicMethods.assign)(Dimps::stageCodes[0], 4);
	*(rVsMode::GetStageCode(mode)) = 0;
}

// False when the main menu is not the one in front.
bool EnterBattle() {
	auto* const root = Dimps::App::GetRootEvent();
	char* query[] = { const_cast<char*>("MainMenu") };
	rMainMenu* const mainMenu = root ? reinterpret_cast<rMainMenu*>(Dimps::Event::EventBaseWithEC::FindForegroundEvent(root, query, 1)) : nullptr;
	if (!mainMenu) return false;
	ProgressData* const progress = *Dimps::GameEvents::RootEvent::GetProgressData(root);
	ProgressData::BattleTypeSettings* const settings = &(ProgressData::GetBattleTypeSettings(progress)[ProgressData::NBT_PVP]);
	*ProgressData::GetNextBattleType(progress) = ProgressData::NBT_PVP;
	settings->editionSelect = TRUE;
	settings->rounds = 1;
	// The longest round the game offers: the test is done long before it.
	settings->timeLimit = { 0, 9999 };
	// The lab runs in a Training battle only; a test run lets it into this one.
	training::AllowOfflineVersusForTest(true);
	GameEvents::VsPreBattle::bSkipToVersus = true;
	GameEvents::VsPreBattle::OnTasksRegistered = SetFighters;
	(rMainMenu::ToItemObserver(mainMenu)->*rMainMenu::itemObserverMethods.GoToVersusMode)();
	return true;
}

bool Submit(Action action, int value = 0, std::vector<training::Input> frames = {}) {
	Command command;
	command.action = action; command.value = value; command.generation = training::ReadView().generation;
	command.frames = std::move(frames);
	return training::Submit(std::move(command));
}

// Player 1 plays the typed moves through a recording slot, facing right.
bool Play(const char* moves) {
	std::vector<std::string> steps;
	std::string error;
	if (!combo::ParseSteps(moves, steps, error)) { spdlog::error("SelfTest: '{}' is not moves: {}", moves, error); return false; }
	return Submit(Action::ClearHistory) && Submit(Action::Load, 0, combo::Synthesize(steps, true, 0)) && Submit(Action::Play);
}

// What the meter shows for Player 1's move: its startup, active and recovery cells.
void Count(const training::MeterView& meter, int& startup, int& active, int& recovery) {
	startup = active = recovery = 0;
	for (const auto& frame : meter.frames) {
		const MeterKind kind = training::ClassifyMeter(frame.fighters[0]);
		startup += kind == MeterKind::Startup; active += kind == MeterKind::Active; recovery += kind == MeterKind::Recovery;
	}
}

}

void TickSelfTest() {
	Run& run = s_run;
	if (!run.read) {
		run.read = true;
		char name[32] = {}, file[1024] = {};
		const DWORD length = GetEnvironmentVariableA("SF4E_SELFTEST", name, sizeof(name));
		if (!length || length >= sizeof(name)) return;
		if (std::string(name) != "training") { spdlog::warn("SelfTest: no test named '{}'", name); return; }
		const DWORD fileLength = GetEnvironmentVariableA("SF4E_SELFTEST_RESULT", file, sizeof(file));
		if (fileLength && fileLength < sizeof(file)) run.resultFile.assign(file, fileLength);
		spdlog::info("SelfTest: training, result to '{}'", run.resultFile);
		Enter(Stage::ToMainMenu);
	}
	if (run.stage == Stage::Off || run.stage == Stage::Closed) return;
	const training::View view = training::ReadView();
	const bool neutral = view.meter.current[0].valid && training::ClassifyStatus(view.meter.current[0].status) == training::Phase::Neutral;
	switch (run.stage) {
	case Stage::Off: case Stage::Closed:
		break;
	case Stage::ToMainMenu:
		// The title screen learns from its Start which device, and so which
		// profile, plays, so a real key is sent from outside
		// (scripts/run-selftest.ps1); nothing here answers the title screen.
		if (AtMainMenu()) {
			Say(true, "main menu");
			if (EnterBattle()) Enter(Stage::ToBattle); else { Say(false, "battle", "the main menu was not in front"); Finish(); }
		}
		else if (Late(180)) { Say(false, "main menu", "never reached"); Finish(); }
		else if (run.waited > 5 * kSecond) {
			// Each arrival at the title screen is said once: the script sends its one Enter on that word.
			const bool title = Screen() == "Title";
			if (title && !run.atTitle) spdlog::info("SelfTest: at the title screen");
			run.atTitle = title;
		}
		break;
	case Stage::ToBattle:
		// The lab is up and the round is being fought, for a second, so the fighters stand.
		if (view.available && view.ready && neutral) { if (++run.quiet > kSecond) { Say(true, "battle", "Ryu against Ryu, the lab is in"); Enter(Stage::Move); } }
		else if (Late(120)) { Say(false, "battle", view.available ? "the round never began" : "the lab never came up in the battle"); Finish(); }
		break;
	case Stage::Move:
		if (run.at == std::size(kCases)) { Enter(Stage::SavePosition); break; }
		if (!Play(kCases[run.at].moves)) { Say(false, kCases[run.at].what, "the lab did not take the moves"); run.at++; break; }
		Enter(Stage::Moving);
		break;
	case Stage::Moving:
		// The playback has to start before its end can be waited for.
		if (view.mode == training::Mode::Playback) Enter(Stage::Settled);
		else if (Late(5)) { Say(false, kCases[run.at].what, "the playback never started"); run.at++; Enter(Stage::Move); }
		break;
	case Stage::Settled: {
		// Played out, and the fighter standing again for half a second.
		if (view.mode == training::Mode::Idle && neutral) run.quiet++; else run.quiet = 0;
		if (run.quiet < kSecond / 2) { if (Late(20)) { Say(false, kCases[run.at].what, "the move never ended"); run.at++; Enter(Stage::Move); } break; }
		const Case& expected = kCases[run.at];
		int startup = 0, active = 0, recovery = 0;
		Count(view.meter, startup, active, recovery);
		const auto& move = view.meter.moves[0];
		const std::string cells = std::to_string(startup) + ", " + std::to_string(active) + " and " + std::to_string(recovery) + " cells";
		const std::string numbers = "startup " + std::to_string(view.meter.startupFrames[0]) + ", active " + std::to_string(move.active) + ", recovery " + std::to_string(move.recovery);
		if (expected.startup < 0) Note(std::string(expected.what) + " (" + expected.moves + "): " + numbers + "; " + cells);
		else {
			const bool numbersRight = view.meter.startupFrames[0] == expected.startup && move.active == expected.active && move.recovery == expected.recovery;
			const bool cellsRight = startup == expected.startup && active == expected.active && recovery == expected.recovery - 1;
			Say(numbersRight, std::string(expected.what) + " numbers", numbers + (numbersRight ? "" :
				", expected " + std::to_string(expected.startup) + "/" + std::to_string(expected.active) + "/" + std::to_string(expected.recovery)));
			Say(cellsRight, std::string(expected.what) + " cells", cells + (cellsRight ? "" :
				", expected " + std::to_string(expected.startup) + ", " + std::to_string(expected.active) + " and " + std::to_string(expected.recovery - 1)));
		}
		run.at++;
		Enter(Stage::Move);
		break;
	}
	case Stage::SavePosition:
		run.saved = view.x[0];
		if (!Submit(Action::Save)) { Say(false, "save position", "the lab did not take the command"); Finish(); break; }
		Enter(Stage::Dash);
		break;
	case Stage::Dash:
		// A forward dash moves Player 1 away from where the position was saved.
		if (!view.checkpoint) { if (Late(5)) { Say(false, "save position", "nothing was saved"); Finish(); } break; }
		if (!Play("66")) { Say(false, "reset position", "the lab did not take the dash"); Finish(); break; }
		Enter(Stage::Dashing);
		break;
	case Stage::Dashing:
		if (view.mode == training::Mode::Idle && neutral && run.waited > kSecond) run.quiet++; else run.quiet = 0;
		if (run.quiet >= kSecond / 2) {
			Note("the dash moved Player 1 from " + std::to_string(run.saved) + " to " + std::to_string(view.x[0]));
			if (view.x[0] - run.saved < 0.1f) { Say(false, "reset position", "the dash did not move the fighter, so the reset proves nothing"); Finish(); break; }
			if (!Submit(Action::Restore)) { Say(false, "reset position", "the lab did not take the command"); Finish(); break; }
			Enter(Stage::Reset);
		}
		else if (Late(20)) { Say(false, "reset position", "the dash never ended"); Finish(); }
		break;
	case Stage::Reset:
		if (++run.waited < kSecond) break;
		Say(view.x[0] - run.saved < 0.01f && run.saved - view.x[0] < 0.01f, "reset position",
			"Player 1 at " + std::to_string(view.x[0]) + ", saved at " + std::to_string(run.saved));
		Finish();
		break;
	case Stage::Closing:
		// A game that does not close on being asked is ended: a test run must not stay up.
		if (Late(20)) { spdlog::warn("SelfTest: the game did not close; ending it"); Enter(Stage::Closed); TerminateProcess(GetCurrentProcess(), run.failed ? 1 : 0); }
		break;
	}
}

} // internal

void InstallSelfTest() {
	char name[32] = {};
	if (!GetEnvironmentVariableA("SF4E_SELFTEST", name, sizeof(name))) return;
	// A test run only. The game counts as in front for the whole run, so once
	// its Start has been pressed the test goes on behind whatever window the
	// player works in.
	BackgroundPlay::KeepActiveForTest();
}

} }
