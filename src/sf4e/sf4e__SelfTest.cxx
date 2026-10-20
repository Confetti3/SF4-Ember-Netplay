#include "sf4e__SelfTest.hxx"

#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Platform.hxx"
#include "../platform/Utf8.hxx"

// What every test shares: the result file, the way to the main menu, the
// outcome and the closing. Each test's steps are in a file of their own and
// start once the main menu is in front.
namespace sf4e { namespace selftest {
namespace {

enum class Stage { Off, ToMainMenu, Testing, Closing };

struct Run : Steps<Stage> {
	bool failed = false;
	std::string screen;
	std::wstring resultFile;
	std::vector<std::string> lines;
} s_run;

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
		Say(false, "result file", "'" + platform::WideToUtf8(s_run.resultFile) + "' cannot be written");
		Finish();
		return;
	}
	spdlog::info("SelfTest: {}, result to '{}'", platform::WideToUtf8(Environment(L"SF4E_SELFTEST")), platform::WideToUtf8(s_run.resultFile));
	s_run.Enter(Stage::ToMainMenu);
}

// The requested test's start and steps.
bool StartTest() {
	switch (Requested()) {
	case Test::Training: return StartTraining();
	case Test::RoomHost: case Test::RoomGuest: return StartRoom();
	case Test::Replay: return StartReplay();
	case Test::TournamentHost: case Test::TournamentGuest: return StartTournament();
	default: return false;
	}
}
void TickTest(bool atMainMenu) {
	switch (Requested()) {
	case Test::Training: TickTraining(); break;
	case Test::RoomHost: case Test::RoomGuest: TickRoom(); break;
	case Test::Replay: TickReplay(atMainMenu); break;
	case Test::TournamentHost: case Test::TournamentGuest: TickTournament(); break;
	default: break;
	}
}

}

std::wstring Environment(const wchar_t* name) {
	const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
	if (!size) return {};
	std::wstring value(size, L'\0');
	value.resize(GetEnvironmentVariableW(name, value.data(), size));
	return value;
}

void Say(bool pass, const std::string& step, const std::string& detail) {
	const std::string line = std::string(pass ? "PASS " : "FAIL ") + step + (detail.empty() ? "" : ": " + detail);
	if (pass) spdlog::info("SelfTest: {}", line); else spdlog::error("SelfTest: {}", line);
	s_run.lines.push_back(line);
	if (!pass) s_run.failed = true;
}

// The outcome is written, and the game asked to close as its updater asks it.
void Finish() {
	s_run.lines.push_back(s_run.failed ? "RESULT fail" : "RESULT pass");
	spdlog::info("SelfTest: {}", s_run.lines.back());
	std::ofstream out(s_run.resultFile, std::ios::trunc);
	for (const std::string& line : s_run.lines) out << line << '\n';
	out.flush();
	if (!out) spdlog::error("SelfTest: the result could not be written to '{}'", platform::WideToUtf8(s_run.resultFile));
	Dimps::Platform::Main::Close();
	s_run.Enter(Stage::Closing);
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
	if (run.stage == Stage::Off) { if (Requested() == Test::None) return; Begin(); }
	run.waited++;
	switch (run.stage) {
	case Stage::ToMainMenu:
		// The title screen learns from its Start which device, and so which
		// profile, plays, so a real key is sent from outside
		// (scripts/run-selftest.ps1); nothing here answers the title screen.
		WatchScreen(screen);
		if (atMainMenu && (Requested() != Test::Replay || ReplayReady())) {
			Say(true, "main menu");
			if (StartTest()) run.Enter(Stage::Testing); else Finish();
		}
		else if (run.Late(180)) { Say(false, "main menu", atMainMenu ? "the game's replay table was never ready: the game may have found no save" : "never reached"); Finish(); }
		break;
	case Stage::Testing:
		TickTest(atMainMenu);
		break;
	case Stage::Closing:
		// A game that does not close on being asked is ended. It ends with 0,
		// as a closed game does, so the launcher shows no recovery screen: the
		// outcome is in the result file.
		if (run.Late(20)) { spdlog::warn("SelfTest: the game did not close; ending it"); TerminateProcess(GetCurrentProcess(), 0); }
		break;
	}
}

} }
