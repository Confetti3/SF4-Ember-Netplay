#include "sf4e__ReplayStore.hxx"

#include <cstdint>
#include <cstring>
#include <string>
#include <windows.h>
#include <detours/detours.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps.hxx"
#include "../Dimps/Dimps__Event.hxx"
#include "../Dimps/Dimps__Game.hxx"
#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplaySlots.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__GameEvents.hxx"

namespace {

using Dimps::Game::ReplayBattle;
using Dimps::Game::SaveDataController;
using Table = Dimps::Game::ReplayInfoList;
using sf4e::replaystore::Step;

// The stream an entry is filled from (Dimps__Game.hxx, ReplayInfoList): four
// words, of which only the base, cursor and size are read.
struct Stream {
	const void* vtable;
	const std::uint8_t* base;
	const std::uint8_t* cursor;
	std::uint32_t size;
};

struct ReplayInfoList : Table {
	BOOL Read(void* stream);
};

struct Entry {
	BOOL Deserialize(Stream* stream);
};

std::uint8_t* s_entries = nullptr;

// Steam's cloud files for this account, as the game itself writes them: the
// game's steam_api.dll exports the accessor, and FileWrite is the first
// function of every ISteamRemoteStorage version.
struct RemoteStorage {
	bool FileWrite(const char* name, const void* data, std::int32_t size);
};

bool WriteThroughSteam(const std::string& name, const sf4e::replayslots::Bytes& contents) {
	using Accessor = RemoteStorage* (*)();
	const HMODULE steam = GetModuleHandleW(L"steam_api.dll");
	const Accessor accessor = steam ? reinterpret_cast<Accessor>(GetProcAddress(steam, "SteamRemoteStorage")) : nullptr;
	RemoteStorage* const storage = accessor ? accessor() : nullptr;
	if (!storage) { spdlog::warn("Replay: Steam's remote storage is not available"); return false; }
	bool (RemoteStorage::* fileWrite)(const char*, const void*, std::int32_t);
	*reinterpret_cast<PVOID*>(&fileWrite) = (*reinterpret_cast<PVOID**>(storage))[0];
	if ((storage->*fileWrite)(name.c_str(), contents.data(), static_cast<std::int32_t>(contents.size()))) return true;
	spdlog::warn("Replay: Steam did not write {} ({} bytes)", name, contents.size());
	return false;
}

BOOL ReplayInfoList::Read(void* stream) {
	const BOOL ok = (this->*publicMethods.Read)(stream);
	std::uint8_t* const begin = GetEntries(this);
	std::uint8_t* const end = GetEntriesEnd(this);
	// A save need not keep the entries in slot order, so each is asked which
	// slot it names.
	const std::ptrdiff_t span = begin ? end - begin : 0;
	bool stock = begin && span == static_cast<std::ptrdiff_t>(sf4e::replayslots::kSlots * EntryBytes);
	int odd = -1;
	for (int at = 0; stock && at < sf4e::replayslots::kSlots; at++) {
		const std::uint32_t slot = GetEntrySlot(GetEntry(begin, at));
		if (slot >= static_cast<std::uint32_t>(sf4e::replayslots::kSlots) && slot != 0xFFFFFFFFu) { stock = false; odd = at; }
	}
	s_entries = stock ? begin : nullptr;
	if (stock) spdlog::info("Replay: the game's replay table is at {}", static_cast<void*>(begin));
	else spdlog::warn("Replay: the game's replay table is not the stock shape (read {}, {} bytes for {} entries of {}, first odd entry {} holds {:#x}); archived replays cannot be added while it runs",
		ok, span, span / static_cast<std::ptrdiff_t>(EntryBytes), EntryBytes, odd, odd >= 0 ? GetEntrySlot(GetEntry(begin, odd)) : 0u);
	return ok;
}

bool SavesBusy() {
	auto* const saves = SaveDataController::staticMethods.GetSingleton();
	return !saves || (saves->*SaveDataController::publicMethods.Busy)();
}

// Puts an archived replay into the game's files and then its table, as the
// newest entry of the match list. The slot it took, or -1.
int Import(const std::wstring& path) {
	sf4e::platform::replays::Imported imported;
	if (!sf4e::platform::replays::ImportFile(path, WriteThroughSteam, imported)) return -1;
	std::uint8_t* entry = Table::GetEntry(s_entries, imported.slot);
	for (int at = 0; at < sf4e::replayslots::kSlots; at++)
		if (Table::GetEntrySlot(Table::GetEntry(s_entries, at)) == static_cast<std::uint32_t>(imported.slot)) { entry = Table::GetEntry(s_entries, at); break; }
	Stream stream{nullptr, imported.record.data(), imported.record.data(), static_cast<std::uint32_t>(imported.record.size())};
	BOOL (Entry::* deserialize)(Stream*);
	*reinterpret_cast<PVOID*>(&deserialize) = Table::GetEntryDeserialize(entry);
	if (!(reinterpret_cast<Entry*>(entry)->*deserialize)(&stream)) {
		// The files already hold the replay; the next game start lists it.
		spdlog::warn("Replay: the game did not take slot {}'s record; it shows after a restart", imported.slot);
		return -1;
	}
	std::memcpy(Table::GetEntrySlotBytes(entry), imported.slotBytes.data(), 2);
	spdlog::info("Replay: slot {} is in the game's table", imported.slot);
	return imported.slot;
}

Dimps::Event::EventBaseWithEC* BattleLogEvent() {
	auto* const root = Dimps::App::GetRootEvent();
	if (!root) return nullptr;
	char* query[1] = { const_cast<char*>("LocalBattleLog") };
	return reinterpret_cast<Dimps::Event::EventBaseWithEC*>(Dimps::Event::EventBaseWithEC::FindForegroundEvent(root, query, 1));
}

// The battle log's current state ("Select", "Versus", "Battle") or nullptr.
Dimps::Event::EventBase* BattleLogState(Dimps::Event::EventBaseWithEC* log) {
	auto* const controller = log ? (log->*Dimps::Event::EventBaseWithEC::publicMethods.GetChildEventController)() : nullptr;
	return controller ? (controller->*Dimps::Event::EventController::publicMethods.GetForegroundEvent)() : nullptr;
}

bool Named(Dimps::Event::EventBase* state, const char* name) { return state && !std::strcmp(Dimps::Event::EventBase::GetName(state), name); }

// The Versus splash waits for its movies and the announcer, which do not
// finish here; it gets the Start press the player could give it
// (Dimps__Game.hxx, ReplayBattle).
void SkipSplash(Dimps::Event::EventBase* versus) {
	ReplayBattle::Splash* const splash = ReplayBattle::GetSplash(versus);
	if (!splash || *ReplayBattle::GetSplashState(splash) != 1) return;
	const auto& native = ReplayBattle::staticMethods;
	auto* const voice = ReplayBattle::GetSplashVoice(splash);
	*ReplayBattle::GetSplashPhase(splash) = 3;
	if (voice) native.FadeVoice(voice, 0x1F);
	*ReplayBattle::GetSplashState(splash) = 3;
	for (int movie = 0; movie < 2; movie++) {
		auto* const m = ReplayBattle::GetSplashMovie(splash, movie);
		if (native.MovieValid(m)) native.MovieSignal(m, "Close", 0);
	}
	spdlog::info("Replay: skipped the Versus splash");
}

// The one operation. waited: ticks in the step's wait. slot: the imported
// replay to play, for Watch, else -1. started: the log has left its list for
// the replay. splash: ticks of the Versus state, -1 once skipped.
constexpr int kPatience = 600, kSplashTicks = 120, kGoneTicks = 120;
struct Operation {
	sf4e::replaystore::Status status;
	int waited = 0, slot = -1, splash = 0;
	bool started = false;
	void Enter(Step step) { status.step = step; waited = 0; }
	void Notice(const char* key, bool error) { status.notice = sf4e::loc::T(key); status.noticeError = error; }
} s_operation;

// Plays the slot's row of the battle log's list, as the list's DECIDE does.
// False while the list has no such row.
bool PlayRow(Dimps::Event::EventBase* select, int slot) {
	ReplayBattle::List* const list = ReplayBattle::GetList(select);
	const int* const rows = list ? ReplayBattle::GetRowsBegin(list) : nullptr;
	const int* const end = list ? ReplayBattle::GetRowsEnd(list) : nullptr;
	for (const int* r = rows; r && r < end; r = ReplayBattle::NextRow(r)) {
		if (*r != slot) continue;
		const int row = ReplayBattle::RowIndex(rows, r);
		*ReplayBattle::GetSelectedRow(list) = row;
		ReplayBattle::staticMethods.PlayRow(list);
		spdlog::info("Replay: playing slot {} from row {} of the battle log", slot, row);
		return true;
	}
	return false;
}

}

void sf4e::replaystore::Install() {
	BOOL (ReplayInfoList::* detour)(void*) = &ReplayInfoList::Read;
	DetourAttach(reinterpret_cast<PVOID*>(&Table::publicMethods.Read), *reinterpret_cast<PVOID*>(&detour));
}

bool sf4e::replaystore::Ready() { return s_entries != nullptr && sf4e::Game::Battle::MatchReplayListWidened(); }

const sf4e::replaystore::Status& sf4e::replaystore::GetStatus() { return s_operation.status; }

void sf4e::replaystore::Start(const replay::Request& request, bool atMainMenu, bool noRoom) {
	Operation& op = s_operation;
	const bool import = request.mode == replay::Mode::Add || request.mode == replay::Mode::Watch;
	const bool jump = request.mode == replay::Mode::Watch || request.mode == replay::Mode::OpenLog;
	if (!import && !jump) return;
	if (op.status.step != Step::Idle || !atMainMenu) { op.Notice("replays.not_ready", true); return; }
	op.slot = -1;
	if (import) {
		if (!Ready() || SavesBusy()) { op.Notice("replays.not_ready", true); return; }
		const std::wstring path = platform::Utf8ToWide(request.path.c_str());
		op.slot = Import(path);
		if (op.slot < 0) { op.Notice("replays.not_added", true); return; }
		platform::replays::MarkWatched(path);
		op.Notice("replays.added", false);
	}
	if (!jump) return;
	if (!noRoom || !GameEvents::MainMenu::OpenLocalBattleLog()) {
		// A replay that was added stays added; only the jump did not happen.
		if (!import) op.Notice("replays.not_ready", true);
		return;
	}
	op.status.logOpens++;
	op.started = false; op.splash = 0;
	op.Enter(Step::OpeningLog);
}

void sf4e::replaystore::Tick(bool atMainMenu) {
	Operation& op = s_operation;
	if (op.status.step == Step::Idle) return;
	auto* const log = BattleLogEvent();
	auto* const state = BattleLogState(log);
	const bool late = ++op.waited > kPatience;
	switch (op.status.step) {
	case Step::OpeningLog:
		// The jump fades through a few frames; the log's list is up soon after.
		if (Named(state, "Select")) op.Enter(op.slot >= 0 ? Step::SelectingRow : Step::InLog);
		else if (late) { spdlog::warn("Replay: the battle log did not come up"); op.Enter(Step::InLog); }
		break;
	case Step::SelectingRow:
		if (Named(state, "Select") && !SavesBusy() && PlayRow(state, op.slot)) op.Enter(Step::Playing);
		else if (late) { spdlog::warn("Replay: slot {} could not be played; it is in the battle log's list", op.slot); op.Enter(Step::InLog); }
		break;
	case Step::Playing:
		if (!log) {
			// The player left the log, or the game moved on without it.
			if (op.waited > kGoneTicks) op.Enter(Step::InLog);
			break;
		}
		if (Named(state, "Versus") || Named(state, "Battle")) {
			op.started = true; op.waited = 0;
			if (Named(state, "Versus") && op.splash >= 0 && ++op.splash > kSplashTicks) { SkipSplash(state); op.splash = -1; }
		}
		else if (Named(state, "Select") && op.started) {
			// Watched, or left: back to the main menu, where Ember reopens.
			GameEvents::MainMenu::LeaveLocalBattleLog();
			op.Enter(Step::InLog);
		}
		else if (late) { spdlog::warn("Replay: the battle log did not start slot {}", op.slot); op.Enter(Step::InLog); }
		break;
	case Step::InLog:
		if (atMainMenu && !log) { op.status.returns++; op.Enter(Step::Idle); }
		break;
	case Step::Idle:
		break;
	}
}
