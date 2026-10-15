#include "sf4e__ReplayStore.hxx"

#include <atomic>
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
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplaySlots.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__Game__Battle.hxx"
#include "sf4e__GameEvents.hxx"
#include "sf4e__ReplayCapture.hxx"
#include "sf4e__ReplayPlayback.hxx"

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
std::atomic<std::uint64_t> s_saveRevision{0};
struct SaveController : SaveDataController { void Start(int channel); };
void SaveController::Start(int channel) {
 ++s_saveRevision;
 (this->*SaveDataController::publicMethods.Start)(channel);
}

// Request a fixed interface version so write, delete and exists have known
// vtable positions, independent of the game's default storage version.
struct RemoteStorage {
	bool FileWrite(const char* name, const void* data, std::int32_t size);
	bool FileDelete(const char* name);
	bool FileExists(const char* name);
};
struct SteamClient {
	RemoteStorage* GetRemoteStorage(int user, int pipe, const char* version);
};

RemoteStorage* Storage() {
	const HMODULE steam = GetModuleHandleW(L"steam_api.dll");
	if (!steam) return nullptr;
	using ClientAccessor = SteamClient* (*)();
	using HandleAccessor = int (*)();
	const auto clientAccessor = reinterpret_cast<ClientAccessor>(GetProcAddress(steam, "SteamClient"));
	const auto user = reinterpret_cast<HandleAccessor>(GetProcAddress(steam, "SteamAPI_GetHSteamUser"));
	const auto pipe = reinterpret_cast<HandleAccessor>(GetProcAddress(steam, "SteamAPI_GetHSteamPipe"));
	SteamClient* const client = clientAccessor ? clientAccessor() : nullptr;
	if (!client || !user || !pipe) return nullptr;
	RemoteStorage* (SteamClient::* get)(int, int, const char*);
	*reinterpret_cast<PVOID*>(&get) = (*reinterpret_cast<PVOID**>(client))[17];
	return (client->*get)(user(), pipe(), "STEAMREMOTESTORAGE_INTERFACE_VERSION016");
}

bool WriteThroughSteam(const std::string& name, const sf4e::replayslots::Bytes& contents) {
	RemoteStorage* const storage = Storage();
	if (!storage) { spdlog::warn("Replay: Steam's remote storage is not available"); return false; }
	bool (RemoteStorage::* fileWrite)(const char*, const void*, std::int32_t);
	*reinterpret_cast<PVOID*>(&fileWrite) = (*reinterpret_cast<PVOID**>(storage))[0];
	if ((storage->*fileWrite)(name.c_str(), contents.data(), static_cast<std::int32_t>(contents.size()))) return true;
	spdlog::warn("Replay: Steam did not write {} ({} bytes)", name, contents.size());
	return false;
}

bool RemoveThroughSteam(const std::string& name) {
	RemoteStorage* const storage = Storage();
	if (!storage) return false;
	bool (RemoteStorage::* exists)(const char*);
	bool (RemoteStorage::* remove)(const char*);
	*reinterpret_cast<PVOID*>(&exists) = (*reinterpret_cast<PVOID**>(storage))[13];
	*reinterpret_cast<PVOID*>(&remove) = (*reinterpret_cast<PVOID**>(storage))[6];
	return !(storage->*exists)(name.c_str()) || (storage->*remove)(name.c_str());
}

BOOL ReplayInfoList::Read(void* stream) {
 ++s_saveRevision;
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

// The table's entry for a slot, by the rule the files' records are found by
// (ReplaySlots.hxx: Record): the entry that names the slot; else the one at
// the slot's position, but only when that one names no slot at all. Null when
// the position holds another slot's entry.
std::uint8_t* EntryOf(int slot) {
	if (!s_entries || slot < 0 || slot >= sf4e::replayslots::kSlots) return nullptr;
	for (int at = 0; at < sf4e::replayslots::kSlots; at++)
		if (Table::GetEntrySlot(Table::GetEntry(s_entries, at)) == static_cast<std::uint32_t>(slot)) return Table::GetEntry(s_entries, at);
	std::uint8_t* const positional = Table::GetEntry(s_entries, slot);
	return Table::GetEntrySlot(positional) < static_cast<std::uint32_t>(sf4e::replayslots::kSlots) ? nullptr : positional;
}

// Puts an archived replay into the game's files and then its table, as the
// newest entry of the match list. `slot` is the one it took. When the table
// does not take the record the files are put back: left ahead of the table,
// the game's next save would write the old record over them.
using sf4e::platform::replays::ImportResult;
ImportResult Import(const sf4e::platform::replays::ImportTransaction& prepared, int& slot, bool watched) {
	slot = -1;
	const auto publish = [](const sf4e::platform::replays::Imported& value) {
		std::uint8_t* const entry = EntryOf(value.slot);
		Stream stream{nullptr, value.record.data(), value.record.data(), static_cast<std::uint32_t>(value.record.size())};
		BOOL (Entry::* deserialize)(Stream*);
		if (entry) *reinterpret_cast<PVOID*>(&deserialize) = Table::GetEntryDeserialize(entry);
		if (!entry || !(reinterpret_cast<Entry*>(entry)->*deserialize)(&stream)) {
			spdlog::warn("Replay: the game's table {} slot {}; its files are put back", entry ? "did not take the record of" : "has no entry for", value.slot);
			return false;
		}
		std::memcpy(Table::GetEntrySlotBytes(entry), value.slotBytes.data(), 2);
		return true;
	};
	const ImportResult result = sf4e::platform::replays::CommitImport(prepared, WriteThroughSteam, RemoveThroughSteam, publish, watched);
	if (result != ImportResult::Done) return result;
	spdlog::info("Replay: slot {} is in the game's table", prepared->imported.slot);
	slot = prepared->imported.slot;
	return ImportResult::Done;
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
// False, doing nothing, while the splash is not yet in the state that takes
// the press.
bool SkipSplash(Dimps::Event::EventBase* versus) {
	ReplayBattle::Splash* const splash = ReplayBattle::GetSplash(versus);
	if (!splash || *ReplayBattle::GetSplashState(splash) != 1) return false;
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
	return true;
}

// The one operation. waited: ticks in the step's wait. slot: the imported
// replay to play, for Watch, else -1. started: the log has left its list for
// the replay. splash: ticks of the Versus state, -1 once skipped. versus:
// ticks in Versus before reaching Battle; the splash and load end inside
// kVersusTicks (thirty seconds) or the replay is given up.
// kDecidedTicks: how long an export shows the decided match (the win pose
// and result) before Ember leaves the replay for the player.
constexpr int kPatience = 600, kSplashTicks = 120, kGoneTicks = 120, kVersusTicks = 1800, kDecidedTicks = 360;
// video: the .mp4 an export writes, empty otherwise; awaited: the capture
// was started and its outcome is owed. cancelled: the player cancelled the
// export, which keeps no video and still leaves the replay once it is decided.
struct Operation {
	sf4e::replaystore::Status status;
	int waited = 0, slot = -1, splash = 0, versus = 0;
	bool started = false, awaited = false, meter = false, cancelled = false;
	// decided: ticks the exported replay's match has been over; -1 once Ember chose to leave.
	int decided = 0;
	std::wstring video;
	// How far the export's replay has played (status.exportFrames, exportTotal).
	sf4e::replay::ExportClock clock;
	sf4e::replay::Request request;
	std::uint64_t saveRevision = 0;
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

void FinishRequest(const sf4e::replay::Request& request, bool noRoom) {
 Operation& op = s_operation;
 const bool exporting = request.mode == sf4e::replay::Mode::Export;
 const bool import = request.mode != sf4e::replay::Mode::OpenLog;
 const bool jump = request.mode != sf4e::replay::Mode::Add;
	if (!jump) return;
	if (!noRoom || !sf4e::GameEvents::MainMenu::OpenLocalBattleLog()) {
		// A replay that was added stays added; only the jump did not happen.
		if (!import) op.Notice("replays.not_ready", true);
		return;
	}
	op.status.logOpens++;
	op.started = false; op.splash = 0; op.versus = 0; op.decided = 0;
	op.clock.Reset(); op.status.exportFrames = op.status.exportTotal = 0;
	if (exporting) {
		// Ember's encoder writes straight next to the replay.
		const std::wstring path = sf4e::platform::Utf8ToWide(request.path.c_str());
		const std::size_t dot = path.find_last_of(L'.');
		op.video = (dot == std::wstring::npos ? path : path.substr(0, dot)) + L".mp4";
		op.status.caption = request.caption;
	}
	op.Enter(Step::OpeningLog);
}

}

void sf4e::replaystore::Install() {
	BOOL (ReplayInfoList::* detour)(void*) = &ReplayInfoList::Read;
	DetourAttach(reinterpret_cast<PVOID*>(&Table::publicMethods.Read), *reinterpret_cast<PVOID*>(&detour));
 void (SaveController::* start)(int) = &SaveController::Start;
 DetourAttach(reinterpret_cast<PVOID*>(&SaveDataController::publicMethods.Start), *reinterpret_cast<PVOID*>(&start));
}

bool sf4e::replaystore::Ready() { return s_entries != nullptr && sf4e::Game::Battle::MatchReplayListWidened(); }

const sf4e::replaystore::Status& sf4e::replaystore::GetStatus() { return s_operation.status; }

void sf4e::replaystore::Start(const replay::Request& request, bool atMainMenu, bool noRoom) {
	Operation& op = s_operation;
	if (request.mode == replay::Mode::CancelExport) {
		// Asked from Ember's menu over the replay, so never at the main menu.
		// Before its battle nothing records, and the export simply ends; after,
		// the capture is told to keep nothing, and Tick reports it once the
		// encoder has gone. A file already closing is kept.
		if (!replay::ExportCancellable(op.status.exportStage)) return;
		if (op.awaited && !replaycapture::Cancel()) return;
		op.cancelled = true;
		spdlog::info("Replay: the export was cancelled");
		if (!op.awaited) { op.video.clear(); op.Notice("export.cancelled", false); }
		return;
	}
	const bool exporting = request.mode == replay::Mode::Export;
	const bool import = request.mode == replay::Mode::Add || request.mode == replay::Mode::Watch || exporting;
	const bool jump = request.mode == replay::Mode::Watch || request.mode == replay::Mode::OpenLog || exporting;
	if (!import && !jump) return;
	if (op.status.step != Step::Idle || !atMainMenu || op.awaited) { op.Notice("replays.not_ready", true); return; }
	op.slot = -1; op.video.clear(); op.meter = request.meter && jump && import; op.cancelled = false;
	if (import) {
		if (!Ready() || SavesBusy()) { op.Notice("replays.not_ready", true); return; }
		const std::wstring path = platform::Utf8ToWide(request.path.c_str());
  op.request = request; op.saveRevision = s_saveRevision.load();
  const auto queued = platform::replays::WantImport(path);
  if (queued != ImportResult::Done) { op.Notice(platform::replays::ImportNotice(queued), true); return; }
  op.Enter(Step::PreparingImport);
  return;
	}
	FinishRequest(request, noRoom);
}

bool sf4e::replaystore::MeterWanted() { return s_operation.meter && s_operation.status.step == Step::Playing; }

const std::string& sf4e::replaystore::PlayingFile() {
	static const std::string none;
	const Operation& op = s_operation;
	return op.status.step == Step::Playing && op.request.mode == replay::Mode::Watch && op.video.empty() ? op.request.path : none;
}

bool sf4e::replaystore::Exporting() {
	const Operation& op = s_operation;
	return !op.video.empty() && (op.status.step == Step::SelectingRow || op.status.step == Step::Playing);
}

bool sf4e::replaystore::ExportPlaying() {
	const Operation& op = s_operation;
	return (!op.video.empty() || op.cancelled) && (op.status.step == Step::SelectingRow || op.status.step == Step::Playing);
}

void sf4e::replaystore::Tick(bool atMainMenu, bool noRoom) {
	Operation& op = s_operation;
	// An export's file closes on the encoder's time; its outcome is the notice.
	if (op.awaited) {
		const replaycapture::State capture = replaycapture::GetState();
		if (capture == replaycapture::State::Done || capture == replaycapture::State::Failed) {
			const std::string file = platform::WideToUtf8(op.video);
			const replay::ExportEnd end = replay::ExportEndOf(capture == replaycapture::State::Done, op.cancelled);
			op.status.notice = capture == replaycapture::State::Done ? loc::Tf(end.key, file) : std::string(loc::T(end.key));
			op.status.noticeError = end.error;
			replaycapture::Clear();
			op.awaited = false; op.video.clear();
		}
	}
	const bool recording = replaycapture::GetState() == replaycapture::State::Recording;
	// Before its battle an export lives only while the log is on its way to it.
	const bool reaching = op.status.step == Step::OpeningLog || op.status.step == Step::SelectingRow || op.status.step == Step::Playing;
	op.status.exportStage = replay::ExportStageOf(!op.video.empty() && (op.awaited || reaching), op.awaited, recording, op.cancelled);
	op.status.captionShown = op.awaited && op.status.step == Step::Playing && op.status.caption.Any() && recording && !op.cancelled;
	// The replay's own clock, as the playback observed the recorder after the
	// last battle update: it stands still while the replay is paused.
	const replaytransport::View& playback = replayplayback::GetView();
	if (op.awaited && recording && playback.playback) {
		op.clock.Observe(playback.round, playback.cursor, playback.roundFrames);
		op.status.exportFrames = op.clock.Played(); op.status.exportTotal = op.clock.Total();
	}
 if (op.status.step == Step::PreparingImport) {
  platform::replays::ImportTransaction prepared;
  if (!platform::replays::TakeImport(prepared)) return;
  op.Enter(Step::Idle);
  // Save-controller start/table reload fence the whole preparation interval.
  // This and the filesystem/account guards are checked before the first write.
  if (!atMainMenu || !Ready() || SavesBusy() || op.saveRevision != s_saveRevision.load()) {
   op.Notice("replays.not_added_yet", true); return;
  }
  const auto result = Import(prepared, op.slot, op.request.mode == replay::Mode::Watch);
  if (result != ImportResult::Done) { op.Notice(platform::replays::ImportNotice(result), true); return; }
  op.Notice("replays.added", false);
  FinishRequest(op.request, noRoom);
  return;
 }
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
			if (op.waited > kGoneTicks) { replaycapture::End(); op.Enter(Step::InLog); }
			break;
		}
		// The fight is loading: record from here to the log's return.
		if (Named(state, "Battle") && !op.video.empty() && !op.awaited) {
			replaycapture::Begin(op.video);
			op.awaited = true;
			spdlog::info("Replay: encoding the playback");
		}
		// An export leaves the replay by itself: once the match has been
		// decided for a while, the end menu's choice is made for the player
		// (Dimps__Game.hxx, ReplayBattle). Written each tick until the state
		// goes, since the menu opens with no choice made. A cancelled export
		// still leaves: the player asked to stop it.
		if (Named(state, "Battle") && (op.awaited || op.cancelled)) {
			using Flow = Dimps::Game::Battle::System;
			const DWORD flow = *Flow::staticVars.CurrentBattleFlow;
			const bool over = flow == Flow::BF__MATCH_RESULT || flow == Flow::BF__MATCH_OVER || flow == Flow::BF__BTL_OVER || flow == Flow::BF__GAME_OVER;
			if (!over && op.decided >= 0) op.decided = 0;
			else if (op.decided >= 0 && ++op.decided > kDecidedTicks) {
				op.decided = -1;
				spdlog::info("Replay: the match is over (battle flow {}); leaving the replay", flow);
			}
			if (op.decided < 0) *reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(state) + ReplayBattle::BattleEndChoice) = ReplayBattle::EndChoiceLeave;
		}
		if (Named(state, "Battle")) { op.started = true; op.waited = 0; op.versus = 0; }
		else if (Named(state, "Versus")) {
			op.started = true; op.waited = 0;
			// The skip is done once it acted; asked again each tick until then.
			if (op.splash >= 0 && ++op.splash > kSplashTicks && SkipSplash(state)) op.splash = -1;
			// Versus is a way to Battle, not a place to stay.
			if (++op.versus > kVersusTicks) {
				spdlog::warn("Replay: slot {} stayed on the Versus screen; left to the player", op.slot);
				op.Enter(Step::InLog);
			}
		}
		else if (Named(state, "Select") && op.started) {
			// Watched, or left: back to the main menu, where Ember reopens.
			replaycapture::End();
			GameEvents::MainMenu::LeaveLocalBattleLog();
			op.Enter(Step::InLog);
		}
		else if (late) { spdlog::warn("Replay: the battle log did not start slot {}", op.slot); op.video.clear(); op.Enter(Step::InLog); }
		break;
	case Step::InLog:
		// Back at the main menu, Ember reopens on the Replays screen. A player
		// who went on into a room from the game's menus is not brought back to it.
		if (!noRoom) op.Enter(Step::Idle);
		else if (atMainMenu && !log) { op.status.returns++; op.Enter(Step::Idle); }
		break;
	case Step::PreparingImport:
	case Step::Idle:
		break;
	}
}
