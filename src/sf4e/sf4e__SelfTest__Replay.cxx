#include "sf4e__SelfTest.hxx"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "../common/Localization.hxx"
#include "../common/ReplaySlots.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__ReplayStore.hxx"

// The replay test: from the main menu, through the requests the Replays
// screen sends (sf4e__ReplayStore.hxx), an import whose third write fails,
// after which every file has to be as it was; an import that succeeds; and a
// replay watched from the battle log and left again. It writes into the
// signed-in account's replay slots as an import does: the replay a slot held
// is in the archive first, as always.
namespace sf4e { namespace selftest {
namespace {

namespace fs = std::filesystem;
namespace slots = replayslots;
using platform::replays::kFirstMatchSlot;
using platform::replays::kLastMatchSlot;
using replaystore::Step;

// Done: the outcome is written (Finish).
enum class Stage { Listing, FailedWrite, Add, Watch, Watching, Done };

struct Replay : Steps<Stage> {
	// The replay that is added and watched, the archive's smallest so the
	// watch is short, and the CRC of the replay inside it; the save files
	// before a step; how often the archive's watched list names it; the
	// frames the battle log has played it.
	std::wstring replay;
	std::uint32_t crc = 0;
	std::map<std::string, std::uint32_t> before;
	int watched = 0, tries = 0, frames = 0;
	std::uint64_t returns = 0;
	bool left = false;
} s_replay;

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

// A request as the Replays screen sends it, run at once on this, the game
// thread. Its outcome is the replay store's notice once its step is Idle.
void Request(replay::Mode mode, bool atMainMenu) {
	replaystore::Start({mode, platform::WideToUtf8(s_replay.replay)}, atMainMenu, true);
}
bool Notice(const char* key) { return replaystore::GetStatus().notice == loc::T(key); }

}

bool ReplayReady() { return replaystore::Ready(); }

bool StartReplay() {
	platform::replays::WantListing();
	s_replay.Enter(Stage::Listing);
	return true;
}

void TickReplay(bool atMainMenu) {
	Replay& run = s_replay;
	run.waited++;
	const replaystore::Status& status = replaystore::GetStatus();
	switch (run.stage) {
	case Stage::Listing: {
		const auto listing = platform::replays::LatestListing();
		if (!listing) {
			if (run.waited % kSecond == 0) platform::replays::WantListing();
			if (run.Late(120)) run.Fail("archive listing", "none after two minutes");
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
		if (run.replay.empty()) { run.Fail("archive listing", "no Ember replay in the archive to test with"); break; }
		Say(true, "archive listing", std::to_string(listing->size()) + " replays; testing with " + platform::WideToUtf8(fs::path(run.replay).filename().wstring()));
		run.before = SaveFiles();
		run.watched = WatchedCount(run.replay);
		if (run.before.empty()) { run.Fail("save folder", "none for the signed-in Steam account"); break; }
		// The third write of the import fails: every file has to be as it was.
		replaystore::FailWriteForTest(2);
		Request(replay::Mode::Add, atMainMenu);
		run.Enter(Stage::FailedWrite);
		break;
	}
	case Stage::FailedWrite:
		if (status.step != Step::Idle) { if (run.Late(60)) run.Fail("failed write", "the import never ended"); break; }
		replaystore::FailWriteForTest(-1);
		// Just after the main menu comes up the game is still reading its saves, and an import is
		// refused until it is done: asked again each second, for half a minute.
		if (Notice("replays.not_ready") && run.tries < 30) {
			if (run.waited < kSecond) break;
			run.tries++;
			replaystore::FailWriteForTest(2);
			Request(replay::Mode::Add, atMainMenu);
			run.Enter(Stage::FailedWrite);
			break;
		}
		if (Notice("replays.not_added_yet")) { run.Fail("failed write", "the game has not saved its last match yet; run again in a minute"); break; }
		if (!Notice("replays.not_added_restored")) Say(false, "failed write", "the import did not report a failure put right: '" + status.notice + "'");
		else if (SaveFiles() != run.before) Say(false, "failed write", "the files are not what they were before the import");
		else Say(true, "failed write", "refused, and every file is as it was");
		Request(replay::Mode::Add, atMainMenu);
		run.Enter(Stage::Add);
		break;
	case Stage::Add: {
		if (status.step != Step::Idle) { if (run.Late(60)) run.Fail("add", "the import never ended"); break; }
		const auto files = SaveFiles();
		if (status.noticeError || !Notice("replays.added")) { run.Fail("add", "'" + status.notice + "'"); break; }
		// Exactly one slot changed, it holds the replay, and its checksum file is the replay's.
		const int slot = SlotChanged(run.before, files);
		const auto file = files.find(std::to_string(slot)), sum = files.find(std::to_string(slot) + ".0");
		if (slot < 0) { run.Fail("add", slot == -2 ? "more than one match slot changed" : "no match slot changed"); break; }
		if (file == files.end() || file->second != run.crc) { run.Fail("add", "slot " + std::to_string(slot) + " changed, but does not hold the replay"); break; }
		Say(sum != files.end(), "add", "in slot " + std::to_string(slot) + ", no other slot touched");
		// Added is not watched.
		Say(WatchedCount(run.replay) == run.watched, "add leaves the replay unwatched");
		run.before = files;
		run.returns = status.returns;
		Request(replay::Mode::Watch, atMainMenu);
		run.Enter(Stage::Watch);
		break;
	}
	case Stage::Watch:
		if (status.step == Step::Idle) run.Fail("watch", "not started: '" + status.notice + "'");
		else run.Enter(Stage::Watching);
		break;
	case Stage::Watching:
		if (status.step == Step::Playing) {
			// A replay ends on a menu that waits for the player. The test has seen what it came
			// for once the battle log has played for half a minute, and leaves as Ember does
			// when the log is back on its list.
			if (++run.frames == 30 * kSecond) { run.left = GameEvents::MainMenu::LeaveLocalBattleLog(); spdlog::info("SelfTest: leaving the replay, {}", run.left ? "asked" : "the battle log is not in front"); }
		}
		if (status.step == Step::Idle) {
			if (!run.frames) Say(false, "watch", "the battle log never played the replay");
			else if (status.returns == run.returns) Say(false, "watch", "the main menu did not come back to Ember");
			else Say(true, "watch", "played and came back after " + std::to_string(run.waited / kSecond) + " s" + (run.left ? ", left by the test" : ""));
			// Watching put it into one more slot and marked it watched, once.
			const int slot = SlotChanged(run.before, SaveFiles());
			Say(slot >= 0, "watch import", slot >= 0 ? "in slot " + std::to_string(slot) : slot == -2 ? "more than one match slot changed" : "no match slot changed");
			Say(WatchedCount(run.replay) == run.watched + 1, "watch marks the replay watched");
			run.Enter(Stage::Done);
			Finish();
		}
		else if (run.Late(600)) run.Fail("watch", std::string("still ") + (run.frames ? "playing" : "starting") + " after ten minutes");
		break;
	case Stage::Done:
		break;
	}
}

} }
