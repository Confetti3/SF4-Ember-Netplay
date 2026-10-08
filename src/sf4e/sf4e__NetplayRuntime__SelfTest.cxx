#include "sf4e__NetplayRuntime.hxx"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <windows.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../common/FocusGate.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplaySlots.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__CodePages.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__ReplayStore.hxx"

// A test that runs in the game itself, for what a unit test cannot reach:
// Steam's own file writes, the game's replay table and its battle log.
// SF4E_SELFTEST=replays starts it; the game is driven through the same
// requests the Replays screen sends, the outcome of every step goes to the
// log and to the file SF4E_SELFTEST_RESULT names, and the game is closed at
// the end. scripts/run-selftest.ps1 starts a build this way and reads the
// result. Without the variable nothing here runs.
//
// It writes into the signed-in account's replay slots as an import does: the
// replay a slot held is in the archive first, as always.
namespace sf4e { namespace NetplayFacade { namespace internal {
namespace {

namespace fs = std::filesystem;
namespace slots = sf4e::replayslots;
using platform::replays::kFirstMatchSlot;
using platform::replays::kLastMatchSlot;
using replaystore::Step;

// Ticks are the runtime's, sixty a second.
constexpr int kSecond = 60;
enum class Stage { Off, ToMainMenu, ToListing, FailedWrite, Add, Watch, Watching, Closing, Closed };

struct Run {
	Stage stage = Stage::Off;
	bool read = false, failed = false;
	int waited = 0;
	std::string resultFile;
	std::vector<std::string> lines;
	// The replay that is added and watched: the archive's smallest, so the
	// watch is short. crc: of the replay inside the file.
	std::wstring replay;
	std::uint32_t crc = 0;
	std::map<std::string, std::uint32_t> before;
	int watched = 0;
	std::uint64_t returns = 0;
	bool played = false, left = false, atTitle = false;
	int tries = 0, playing = 0;
} s_run;

void Say(bool pass, const char* step, const std::string& detail = {}) {
	const std::string line = std::string(pass ? "PASS " : "FAIL ") + step + (detail.empty() ? "" : ": " + detail);
	if (pass) spdlog::info("SelfTest: {}", line); else spdlog::error("SelfTest: {}", line);
	s_run.lines.push_back(line);
	if (!pass) s_run.failed = true;
}

void Enter(Stage stage) { s_run.stage = stage; s_run.waited = 0; }

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

std::uint32_t FileCrc(const fs::path& file) {
	std::ifstream in(file, std::ios::binary);
	const slots::Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return slots::Crc32(bytes.data(), bytes.size());
}

// Every file an import may touch, by its CRC: the two indexes, their
// checksum files, and each match slot with its own.
std::map<std::string, std::uint32_t> SaveFiles() {
	std::map<std::string, std::uint32_t> files;
	const fs::path saves = platform::replays::FindFolders().active;
	if (saves.empty()) return files;
	std::vector<std::string> names = {"LIST", "LIST.0", "replays-swan.dat", "replays-swan.dat.0"};
	for (int slot = kFirstMatchSlot; slot <= kLastMatchSlot; slot++) { names.push_back(std::to_string(slot)); names.push_back(std::to_string(slot) + ".0"); }
	std::error_code ignored;
	for (const std::string& name : names) if (fs::exists(saves / name, ignored)) files[name] = FileCrc(saves / name);
	return files;
}

// The one match slot whose file an import changed, -1 when none did or more
// than one, and -2 when a file changed that an import of that slot may not
// touch: another slot, or anything but the slot, its checksum file and the
// two indexes with theirs.
int SlotChanged(const std::map<std::string, std::uint32_t>& before, const std::map<std::string, std::uint32_t>& after) {
	int changed = -1;
	for (int slot = kFirstMatchSlot; slot <= kLastMatchSlot; slot++) {
		const std::string name = std::to_string(slot);
		const auto was = before.find(name), is = after.find(name);
		const bool same = (was == before.end() && is == after.end()) || (was != before.end() && is != after.end() && was->second == is->second);
		const auto wasSum = before.find(name + ".0"), isSum = after.find(name + ".0");
		const bool sameSum = (wasSum == before.end() && isSum == after.end()) || (wasSum != before.end() && isSum != after.end() && wasSum->second == isSum->second);
		if (same && sameSum) continue;
		if (changed >= 0) return -2;
		changed = slot;
	}
	return changed;
}

// How often the archive's watched list names this file.
int WatchedCount(const std::wstring& replay) {
	const fs::path archive = platform::replays::FindFolders().archive;
	if (archive.empty()) return 0;
	std::ifstream in(archive / L"watched.txt");
	const std::string name = platform::WideToUtf8(fs::path(replay).filename().wstring());
	int count = 0;
	for (std::string line; std::getline(in, line);) count += line == name;
	return count;
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

}

// Whether the game was made to run behind other windows; said once the log exists.
bool s_runsBehind = false;

// What the game's frame takes for the foreground window in a test run: its
// own, so the game keeps running whatever window the player has in front.
HWND WINAPI SelfTestForeground() {
	auto* const main = Dimps::Platform::Main::staticMethods.GetSingleton();
	const auto* const data = main ? *Dimps::Platform::Main::GetWindowData(main) : nullptr;
	return data && data->hWnd ? data->hWnd : GetForegroundWindow();
}
// The frame's `call dword ptr [...]` reads its function from here.
HWND (WINAPI* selfTestForeground)() = SelfTestForeground;

} // internal

void InstallSelfTest() {
	char name[32] = {};
	if (!GetEnvironmentVariableA("SF4E_SELFTEST", name, sizeof(name))) return;
	// A test run only: a game started without the variable keeps its own check.
	// The game counts as in front for the whole run, so once its Start has been
	// pressed the test goes on behind whatever window the player works in.
	const focus_gate::Edit edits[1] = { focus_gate::CallThrough(Dimps::App::activeFocusCheck, Dimps::App::foregroundWindowImport,
		static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&internal::selfTestForeground))) };
	CodePages pages;
	internal::s_runsBehind = focus_gate::ApplyAll(edits, pages);
}

namespace internal {
void TickSelfTest() {
	Run& run = s_run;
	if (!run.read) {
		run.read = true;
		char name[32] = {}, file[1024] = {};
		const DWORD length = GetEnvironmentVariableA("SF4E_SELFTEST", name, sizeof(name));
		if (!length || length >= sizeof(name)) return;
		if (std::string(name) != "replays") { spdlog::warn("SelfTest: no test named '{}'", name); return; }
		const DWORD fileLength = GetEnvironmentVariableA("SF4E_SELFTEST_RESULT", file, sizeof(file));
		if (fileLength && fileLength < sizeof(file)) run.resultFile.assign(file, fileLength);
		spdlog::info("SelfTest: replays, result to '{}'; the game {} behind other windows", run.resultFile, s_runsBehind ? "runs" : "does NOT run");
		Enter(Stage::ToMainMenu);
	}
	const replaystore::Status& status = replaystore::GetStatus();
	switch (run.stage) {
	case Stage::Off: case Stage::Closed:
		break;
	case Stage::ToMainMenu:
		// The title screen learns from its Start which device, and so which
		// profile, plays: a Start from no device finds no save. So a real key is
		// sent from outside (scripts/run-selftest.ps1), and nothing here answers
		// the title screen or anything it shows.
		if (AtMainMenu() && replaystore::Ready()) { Say(true, "main menu"); platform::replays::WantListing(); Enter(Stage::ToListing); }
		// A minute after its Start the game is at the main menu, or it is showing something
		// the test must not answer; the game is closed and the run fails.
		else if (Late(180)) { Say(false, "main menu", AtMainMenu() ? "the game's replay table was never ready: the game may have found no save" : "never reached"); Finish(); }
		else if (run.waited > 5 * kSecond) {
			// Each arrival at the title screen is said once: the script sends its one Enter on that word.
			const bool title = Screen() == "Title";
			if (title && !run.atTitle) spdlog::info("SelfTest: at the title screen");
			run.atTitle = title;
		}
		else if (run.waited % (20 * kSecond) == 0) spdlog::info("SelfTest: waiting for the main menu, {} s", run.waited / kSecond);
		break;
	case Stage::ToListing: {
		const auto listing = platform::replays::LatestListing();
		if (!listing) {
			if (run.waited % kSecond == 0) platform::replays::WantListing();
			if (Late(120)) { Say(false, "archive listing", "none after two minutes"); Finish(); }
			break;
		}
		std::uintmax_t smallest = 0;
		for (const auto& replay : *listing) {
			std::uint64_t time = 0; std::uint32_t crc = 0;
			std::error_code failed;
			const std::uintmax_t size = fs::file_size(replay.path, failed);
			if (failed || !slots::ParseArchiveName(replay.path.filename().wstring(), time, crc) || (!run.replay.empty() && size >= smallest)) continue;
			run.replay = replay.path.wstring(); run.crc = crc; smallest = size;
		}
		if (run.replay.empty()) { Say(false, "archive listing", "no Ember replay in the archive to test with"); Finish(); break; }
		Say(true, "archive listing", std::to_string(listing->size()) + " replays; testing with " + platform::WideToUtf8(fs::path(run.replay).filename().wstring()));
		run.before = SaveFiles();
		run.watched = WatchedCount(run.replay);
		if (run.before.empty()) { Say(false, "save folder", "none for the signed-in Steam account"); Finish(); break; }
		// The third write of the import fails: every file has to be as it was.
		replaystore::FailWriteForTest(2);
		RunReplayRequest({replay::Mode::Add, platform::WideToUtf8(run.replay)});
		Enter(Stage::FailedWrite);
		break;
	}
	case Stage::FailedWrite: {
		replaystore::FailWriteForTest(-1);
		// Just after the main menu comes up the game is still reading its saves, and an import is
		// refused until it is done: asked again each second, for half a minute.
		if (status.noticeError && status.notice == loc::T("replays.not_ready") && run.tries < 30) {
			if (++run.waited < kSecond) break;
			run.tries++; run.waited = 0;
			replaystore::FailWriteForTest(2);
			RunReplayRequest({replay::Mode::Add, platform::WideToUtf8(run.replay)});
			break;
		}
		const bool refused = status.noticeError && status.notice == loc::T("replays.not_added_files");
		const bool waits = status.noticeError && status.notice == loc::T("replays.not_added_yet");
		if (waits) { Say(false, "failed write", "the game has not saved its last match yet; run again in a minute"); Finish(); break; }
		if (!refused) Say(false, "failed write", "the import did not report a file failure: '" + status.notice + "'");
		else if (SaveFiles() != run.before) Say(false, "failed write", "the files are not what they were before the import");
		else Say(true, "failed write", "refused, and every file is as it was");
		RunReplayRequest({replay::Mode::Add, platform::WideToUtf8(run.replay)});
		Enter(Stage::Add);
		break;
	}
	case Stage::Add: {
		const auto files = SaveFiles();
		if (status.noticeError || status.notice != loc::T("replays.added")) { Say(false, "add", "'" + status.notice + "'"); Finish(); break; }
		// Exactly one slot changed, it holds the replay, and its checksum file is the replay's.
		const int slot = SlotChanged(run.before, files);
		const auto file = files.find(std::to_string(slot)), sum = files.find(std::to_string(slot) + ".0");
		if (slot < 0) { Say(false, "add", slot == -2 ? "more than one match slot changed" : "no match slot changed"); Finish(); break; }
		if (file == files.end() || file->second != run.crc) { Say(false, "add", "slot " + std::to_string(slot) + " changed, but does not hold the replay"); Finish(); break; }
		Say(sum != files.end(), "add", "in slot " + std::to_string(slot) + ", no other slot touched");
		// Added is not watched.
		Say(WatchedCount(run.replay) == run.watched, "add leaves the replay unwatched");
		run.before = files;
		run.returns = status.returns;
		RunReplayRequest({replay::Mode::Watch, platform::WideToUtf8(run.replay)});
		Enter(Stage::Watch);
		break;
	}
	case Stage::Watch:
		if (status.step == Step::Idle) { Say(false, "watch", "not started: '" + status.notice + "'"); Finish(); }
		else Enter(Stage::Watching);
		break;
	case Stage::Watching:
		if (status.step == Step::Playing) {
			run.played = true;
			// A replay ends on a menu that waits for the player. The test has seen what it came
			// for once the battle log has played for half a minute, and leaves as Ember does
			// when the log is back on its list.
			if (++run.playing == 30 * kSecond) { run.left = GameEvents::MainMenu::LeaveLocalBattleLog(); spdlog::info("SelfTest: leaving the replay, {}", run.left ? "asked" : "the battle log is not in front"); }
		}
		if (status.step == Step::Idle) {
			if (!run.played) Say(false, "watch", "the battle log never played the replay");
			else if (status.returns == run.returns) Say(false, "watch", "the main menu did not come back to Ember");
			else Say(true, "watch", "played and came back after " + std::to_string(run.waited / kSecond) + " s" + (run.left ? ", left by the test" : ""));
			// Watching put it into one more slot and marked it watched, once.
			const int slot = SlotChanged(run.before, SaveFiles());
			Say(slot >= 0, "watch import", slot >= 0 ? "in slot " + std::to_string(slot) : slot == -2 ? "more than one match slot changed" : "no match slot changed");
			Say(WatchedCount(run.replay) == run.watched + 1, "watch marks the replay watched");
			Finish();
		}
		else if (Late(600)) { Say(false, "watch", std::string("still ") + (run.played ? "playing" : "starting") + " after ten minutes"); Finish(); }
		break;
	case Stage::Closing:
		// A game that does not close on being asked is ended: a test run must not stay up.
		if (Late(20)) { spdlog::warn("SelfTest: the game did not close; ending it"); Enter(Stage::Closed); TerminateProcess(GetCurrentProcess(), run.failed ? 1 : 0); }
		break;
	}
}

} } }
