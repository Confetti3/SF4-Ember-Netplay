#include "sf4e__SelfTest.hxx"

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__GameEvents.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../platform/Utf8.hxx"
#include "../training/MoveInputs.hxx"
#include "../training/TrainingRuntime.hxx"
#include "sf4e__GameEvents.hxx"

// The training test: the frame meter against moves whose frame data is known,
// typed in numpad notation and played by the lab, a recording and its
// playback, and the lab's saved position. An offline Versus battle of Ryu
// against Ryu is entered the way a room's match is, with no menu, and the lab
// is let into that battle. It uses only what the lab's own controls send
// (training::Submit) and reads.
namespace sf4e { namespace selftest {
namespace {

using training::Action;
using training::Mode;

// Ticks are the runtime's, sixty a second.
constexpr int kSecond = 60;
// Frames the test records before it stops the recording.
constexpr int kRecorded = 8 * kSecond;
enum class Stage { Off, ToMainMenu, ToBattle, Move, Starting, Settling, Measure, Record, Standby, Recording, Replaying, SavePosition, Dash, Dashed, Reset, Closing };

// Ryu's crouching LP: startup 3, active 2, recovery 7. The numbers on the
// meter's line have to be these. Its cells follow the startup number
// (FrameMeter.hxx: ClassifyMeter), so the recovery has one cell less.
constexpr int kStartup = 3, kActive = 2, kRecovery = 7;

struct Run {
	Stage stage = Stage::Off;
	// Where a played move goes once it has ended, and where it goes if it does not.
	Stage then = Stage::Off, otherwise = Stage::Off;
	std::string playing;
	bool failed = false;
	// Ticks in this stage, and the frames its condition has held in a row.
	int waited = 0, held = 0;
	// Frames recorded, and the furthest frame the playback was seen at.
	int recorded = 0, reached = 0;
	float saved = 0, dashed = 0;
	std::string screen;
	std::wstring resultFile;
	std::vector<std::string> lines;
} s_run;

std::wstring Environment(const wchar_t* name) {
	const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
	if (!size) return {};
	std::wstring value(size, L'\0');
	value.resize(GetEnvironmentVariableW(name, value.data(), size));
	return value;
}

void Say(bool pass, const std::string& step, const std::string& detail = {}) {
	const std::string line = std::string(pass ? "PASS " : "FAIL ") + step + (detail.empty() ? "" : ": " + detail);
	if (pass) spdlog::info("SelfTest: {}", line); else spdlog::error("SelfTest: {}", line);
	s_run.lines.push_back(line);
	if (!pass) s_run.failed = true;
}

void Enter(Stage stage) { s_run.stage = stage; s_run.waited = 0; s_run.held = 0; }
bool Late(int seconds) { return s_run.waited > seconds * kSecond; }

// The outcome is written, and the game asked to close as its updater asks it.
void Finish() {
	s_run.lines.push_back(s_run.failed ? "RESULT fail" : "RESULT pass");
	spdlog::info("SelfTest: {}", s_run.lines.back());
	std::ofstream out(s_run.resultFile, std::ios::trunc);
	for (const std::string& line : s_run.lines) out << line << '\n';
	out.flush();
	if (!out) spdlog::error("SelfTest: the result could not be written to '{}'", platform::WideToUtf8(s_run.resultFile));
	Dimps::Platform::Main::Close();
	Enter(Stage::Closing);
}

// A failed step goes on at `otherwise`, or ends the run when that is Closing.
void Fail(const std::string& step, const std::string& detail, Stage otherwise) {
	Say(false, step, detail);
	if (otherwise == Stage::Closing) Finish(); else Enter(otherwise);
}

// Ryu against Ryu on the first stage.
void SetFighters() {
	Dimps::GameEvents::VsMode::ConfirmedCharaConditions ryu{};
	ryu.charaID = 0; ryu.unc_edition = Dimps::Game::Battle::ED_USF4;
	const Dimps::GameEvents::VsMode::ConfirmedCharaConditions sides[2] = { ryu, ryu };
	if (!GameEvents::VsPreBattle::SetSides(sides, 0)) spdlog::error("SelfTest: VS mode is not in front; the fighters were not set");
}

// False when the main menu is not the one in front.
bool EnterBattle() {
	GameEvents::VsPreBattle::VersusSettings settings;
	settings.editionSelect = TRUE;
	// The longest round the game offers: the test is done long before it.
	settings.timeLimit = { 0, 9999 };
	// No replay is saved, however the battle ends: the test writes nothing to the saves.
	settings.recordReplay = FALSE;
	GameEvents::VsBattle::nextRequest = {};
	// The lab runs in a Training battle only; a test run lets it into this one.
	training::AllowOfflineVersusForTest();
	return GameEvents::VsPreBattle::GoToVersus(settings, SetFighters);
}

// loop: for Action::Loop, whether playback repeats.
bool Submit(Action action, std::vector<training::Input> frames = {}, bool loop = false) {
	training::Command command;
	command.action = action; command.generation = training::ReadView().generation;
	command.frames = std::move(frames); command.side = 0; command.loop = loop;
	return training::Submit(std::move(command));
}

// Player 1 plays the typed moves through the selected slot, facing right.
// A test run is a game of its own, so that slot is the only one with
// anything in it and Play plays it. Once the playback has begun, ended, and
// the fighter has stood for half a second, the test goes on at `then`.
void Play(const char* moves, const std::string& what, Stage then, Stage otherwise) {
	s_run.playing = what; s_run.then = then; s_run.otherwise = otherwise;
	std::vector<std::string> steps;
	combo::Compiled compiled;
	std::string error;
	if (!combo::ParseSteps(moves, steps, error) || !combo::Synthesize(steps, true, compiled, error)) { Fail(what, std::string("'") + moves + "' is not moves: " + error, otherwise); return; }
	if (!Submit(Action::ClearHistory) || !Submit(Action::Load, std::move(compiled.frames)) || !Submit(Action::Play)) { Fail(what, "the lab did not take the moves", otherwise); return; }
	Enter(Stage::Starting);
}

void Measure(const training::View& view) {
	const Cells cells = CountCells(view.meter, 0);
	const auto& move = view.meter.moves[0];
	const bool numbersRight = view.meter.startupFrames[0] == kStartup && move.active == kActive && move.recovery == kRecovery;
	const bool cellsRight = cells.startup == kStartup && cells.active == kActive && cells.recovery == kRecovery - 1;
	Say(numbersRight, "Ryu crouching LP numbers", "startup " + std::to_string(view.meter.startupFrames[0]) + ", active " +
		std::to_string(move.active) + ", recovery " + std::to_string(move.recovery) + (numbersRight ? "" : ", expected 3/2/7"));
	Say(cellsRight, "Ryu crouching LP cells", std::to_string(cells.startup) + ", " + std::to_string(cells.active) + " and " +
		std::to_string(cells.recovery) + " cells" + (cellsRight ? "" : ", expected 3, 2 and 6"));
}

// Every change of the screen in front, arrival and departure alike, goes to
// the log: the script sends its one Enter only while the newest line names the
// title screen.
void WatchScreen(const std::string& screen) {
	if (screen == s_run.screen) return;
	s_run.screen = screen;
	spdlog::info("SelfTest: screen {}", screen.empty() ? "none" : screen);
}

void Begin() {
	s_run.resultFile = Environment(L"SF4E_SELFTEST_RESULT");
	// A run that cannot leave its outcome is not run: the game closes at once.
	std::ofstream probe(s_run.resultFile, std::ios::trunc);
	if (s_run.resultFile.empty() || !probe) {
		Fail("result file", "'" + platform::WideToUtf8(s_run.resultFile) + "' cannot be written", Stage::Closing);
		return;
	}
	spdlog::info("SelfTest: training, result to '{}'", platform::WideToUtf8(s_run.resultFile));
	Enter(Stage::ToMainMenu);
}

}

Test Requested() {
	static const Test test = [] {
		const std::wstring name = Environment(L"SF4E_SELFTEST");
		const Test parsed = Parse(name);
		if (!name.empty() && parsed == Test::None) spdlog::warn("SelfTest: no test named '{}'", platform::WideToUtf8(name));
		return parsed;
	}();
	return test;
}

void Tick(bool atMainMenu, const std::string& screen) {
	Run& run = s_run;
	if (run.stage == Stage::Off) { if (Requested() != Test::Training) return; Begin(); }
	run.waited++;
	const training::View view = training::ReadView();
	const bool neutral = view.meter.current[0].valid && training::ClassifyStatus(view.meter.current[0].status) == training::Phase::Neutral;
	switch (run.stage) {
	case Stage::Off:
		break;
	case Stage::ToMainMenu:
		// The title screen learns from its Start which device, and so which
		// profile, plays, so a real key is sent from outside
		// (scripts/run-selftest.ps1); nothing here answers the title screen.
		WatchScreen(screen);
		if (atMainMenu) {
			Say(true, "main menu");
			if (EnterBattle()) Enter(Stage::ToBattle); else Fail("battle", "the main menu was not in front", Stage::Closing);
		}
		else if (Late(180)) Fail("main menu", "never reached", Stage::Closing);
		break;
	case Stage::ToBattle:
		// The lab is up and the fighter has stood for a second, so the round is being fought.
		if (view.available && view.ready && neutral) run.held++; else run.held = 0;
		if (run.held > kSecond) {
			Say(true, "battle", "Ryu against Ryu, the lab is in");
			// Playback repeats by default; every step here plays once.
			if (Submit(Action::Loop)) Enter(Stage::Move); else Fail("battle", "the lab did not take Loop off", Stage::Closing);
		}
		else if (Late(120)) Fail("battle", view.available ? "the round never began" : "the lab never came up in the battle", Stage::Closing);
		break;
	case Stage::Move:
		Play("2LP", "Ryu crouching LP", Stage::Measure, Stage::Record);
		break;
	case Stage::Starting:
		// The playback has to begin before its end can be waited for.
		if (view.mode == Mode::Playback) Enter(Stage::Settling);
		else if (Late(5)) Fail(run.playing, "the playback never started", run.otherwise);
		break;
	case Stage::Settling:
		if (view.mode == Mode::Idle && neutral) run.held++; else run.held = 0;
		if (run.held >= kSecond / 2) Enter(run.then);
		else if (Late(20)) Fail(run.playing, "the move never ended", run.otherwise);
		break;
	case Stage::Measure:
		Measure(view);
		Enter(Stage::Record);
		break;
	case Stage::Record:
		// The lab's record control: standby first, then recording, then stop.
		// Eight seconds are recorded and played back for five, long enough for
		// the HUD's strip to be seen.
		if (Submit(Action::AdvanceRecording)) Enter(Stage::Standby); else Fail("record", "the lab did not take the command", Stage::SavePosition);
		break;
	case Stage::Standby:
		if (view.mode != Mode::Standby) { if (Late(5)) Fail("record", "standby never began", Stage::SavePosition); break; }
		if (Submit(Action::AdvanceRecording)) { run.recorded = 0; Enter(Stage::Recording); } else Fail("record", "the lab did not take the command", Stage::SavePosition);
		break;
	case Stage::Recording:
		if (view.mode == Mode::Recording && run.recorded < kRecorded && ++run.recorded == kRecorded) Submit(Action::AdvanceRecording);
		else if (run.recorded == kRecorded && view.mode == Mode::Idle) {
			const int length = view.lengths[view.selected];
			Say(length >= kRecorded - kSecond && length <= kRecorded + kSecond, "record", std::to_string(length) + " frames in slot " + std::to_string(view.selected + 1));
			if (Submit(Action::Play)) { run.reached = 0; Enter(Stage::Replaying); } else Fail("play recording", "the lab did not take the command", Stage::SavePosition);
		}
		else if (Late(20)) { Submit(Action::Stop); Fail("record", "the recording never started or never stopped", Stage::SavePosition); }
		break;
	case Stage::Replaying:
		if (view.mode == Mode::Playback && view.cursor > run.reached) run.reached = view.cursor;
		if (!Late(5)) break;
		Say(run.reached > 2 * kSecond, "play recording", "reached frame " + std::to_string(run.reached) + " of " + std::to_string(view.lengths[view.selected]));
		Submit(Action::Stop);
		Enter(Stage::SavePosition);
		break;
	case Stage::SavePosition:
		// The playback just stopped has to be over before the position is saved.
		if (view.mode != Mode::Idle) { if (Late(5)) Fail("save position", "the playback never stopped", Stage::Closing); break; }
		run.saved = view.x[0];
		if (Submit(Action::Save)) Enter(Stage::Dash); else Fail("save position", "the lab did not take the command", Stage::Closing);
		break;
	case Stage::Dash:
		// A forward dash moves Player 1 away from where the position was saved.
		if (view.checkpoint) Play("66", "reset position", Stage::Dashed, Stage::Closing);
		else if (Late(5)) Fail("save position", "nothing was saved", Stage::Closing);
		break;
	case Stage::Dashed:
		run.dashed = view.x[0];
		if (run.dashed - run.saved < 0.1f) Fail("reset position", "the dash did not move the fighter, so the reset proves nothing", Stage::Closing);
		else if (Submit(Action::Restore)) Enter(Stage::Reset);
		else Fail("reset position", "the lab did not take the command", Stage::Closing);
		break;
	case Stage::Reset:
		if (!Late(1)) break;
		Say(std::fabs(view.x[0] - run.saved) < 0.01f, "reset position", "Player 1 at " + std::to_string(view.x[0]) +
			", saved at " + std::to_string(run.saved) + ", dashed to " + std::to_string(run.dashed));
		Finish();
		break;
	case Stage::Closing:
		// A game that does not close on being asked is ended. It ends with 0,
		// as a closed game does, so the launcher shows no recovery screen: the
		// outcome is in the result file.
		if (Late(20)) { spdlog::warn("SelfTest: the game did not close; ending it"); TerminateProcess(GetCurrentProcess(), 0); }
		break;
	}
}

} }
