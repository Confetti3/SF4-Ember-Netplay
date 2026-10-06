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
#include "../common/ReplaySlots.hxx"
#include "../platform/ReplayFiles.hxx"

namespace {

// The game's replay slots (Dimps__Game.hxx): a std::vector of 310 entries at
// +8 (begin) and +0xC (end), 0x108 bytes each. An entry is its vtable, then
// what its slot record holds (slot number at +4, used at +0xC, CRC, size,
// 64-bit save time, title, and the menu's fields from +0x38), then the slot's
// two bytes at +0x106. Its vtable's third function (0x676820) fills it from
// a memory stream holding the 125-byte record: the stream is four words, of
// which only the base, cursor and size are read.
constexpr std::size_t kEntryBytes = 0x108, kEntrySlotBytesOffset = 0x106;

struct Stream {
	const void* vtable;
	const std::uint8_t* base;
	const std::uint8_t* cursor;
	std::uint32_t size;
};

struct ReplayInfoList : Dimps::Game::ReplayInfoList {
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
	std::uint8_t* const begin = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(this) + 8);
	std::uint8_t* const end = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(this) + 0xC);
	bool stock = begin && end - begin == static_cast<std::ptrdiff_t>(sf4e::replayslots::kSlots * kEntryBytes);
	for (int slot = 0; stock && slot < sf4e::replayslots::kSlots; slot++) {
		stock = sf4e::replayslots::ReadU32(begin + slot * kEntryBytes + 4) == static_cast<std::uint32_t>(slot);
	}
	s_entries = stock ? begin : nullptr;
	if (stock) spdlog::info("Replay: the game's replay table is at {}", static_cast<void*>(begin));
	else spdlog::warn("Replay: the game's replay table is not the stock shape; archived replays cannot be added while it runs");
	return ok;
}

}

void sf4e::replaystore::Install() {
	BOOL (ReplayInfoList::* detour)(void*) = &ReplayInfoList::Read;
	DetourAttach(reinterpret_cast<PVOID*>(&ReplayInfoList::publicMethods.Read), *reinterpret_cast<PVOID*>(&detour));
}

bool sf4e::replaystore::Ready() { return s_entries != nullptr; }

namespace {
enum class Playback { Idle, Waiting, Started };
Playback s_playback = Playback::Idle;
int s_playbackWait = 0;

Dimps::Event::EventBaseWithEC* BattleLogEvent() {
	auto* const root = Dimps::App::GetRootEvent();
	if (!root) return nullptr;
	char* query[1] = { const_cast<char*>("LocalBattleLog") };
	return reinterpret_cast<Dimps::Event::EventBaseWithEC*>(Dimps::Event::EventBaseWithEC::FindForegroundEvent(root, query, 1));
}

// The battle log's current state ("Select", "Versus", "Battle") or nullptr.
Dimps::Event::EventBase* BattleLogState(Dimps::Event::EventBaseWithEC* log) {
	auto* const controller = (log->*Dimps::Event::EventBaseWithEC::publicMethods.GetChildEventController)();
	return controller ? (controller->*Dimps::Event::EventController::publicMethods.GetForegroundEvent)() : nullptr;
}

const char* StateName(Dimps::Event::EventBase* state) { return state ? Dimps::Event::EventBase::GetName(state) : ""; }
}

bool sf4e::replaystore::TickPlayback(const Playable& playable) {
	using Dimps::Game::ReplayBattle;
	using Dimps::Game::SaveDataController;
	if (s_playback == Playback::Idle) { s_playback = Playback::Waiting; s_playbackWait = 0; }
	if (s_playback == Playback::Started) return true;
	auto* const log = BattleLogEvent();
	auto* const select = log ? BattleLogState(log) : nullptr;
	// The jump fades through a few frames; the log's list is up soon after.
	if (!select || std::strcmp(StateName(select), "Select")) {
		if (++s_playbackWait > 600) { spdlog::warn("Replay: the battle log did not come up; the replay is in its list"); s_playback = Playback::Idle; return true; }
		return false;
	}
	auto* const saves = SaveDataController::staticMethods.GetSingleton();
	if ((saves->*SaveDataController::publicMethods.Busy)()) {
		if (++s_playbackWait > 600) { spdlog::warn("Replay: the save controller stayed busy; the replay is in the battle log's list"); s_playback = Playback::Idle; return true; }
		return false;
	}
	// The list is the Select event's, with the imported slot among its rows;
	// playing it is what the list's DECIDE does (Dimps__Game.hxx, ReplayBattle).
	auto* const list = *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(select) + ReplayBattle::SelectList);
	const std::uint8_t* const rows = list ? *reinterpret_cast<std::uint8_t**>(list + ReplayBattle::ListRowsBegin) : nullptr;
	const std::uint8_t* const end = list ? *reinterpret_cast<std::uint8_t**>(list + ReplayBattle::ListRowsEnd) : nullptr;
	int row = -1;
	for (const std::uint8_t* r = rows; r && r < end; r += ReplayBattle::ListRowBytes) {
		if (*reinterpret_cast<const int*>(r) == playable.slot) { row = static_cast<int>((r - rows) / ReplayBattle::ListRowBytes); break; }
	}
	if (row < 0) {
		if (++s_playbackWait > 600) { spdlog::warn("Replay: slot {} is not in the battle log's list", playable.slot); s_playback = Playback::Idle; return true; }
		return false;
	}
	*reinterpret_cast<int*>(list + ReplayBattle::ListSelected) = row;
	ReplayBattle::staticMethods.PlayRow(reinterpret_cast<ReplayBattle::List*>(list));
	spdlog::info("Replay: playing slot {} from row {} of the battle log", playable.slot, row);
	s_playback = Playback::Started;
	s_playbackWait = 0;
	return true;
}

bool sf4e::replaystore::PlaybackOver() {
	if (s_playback != Playback::Started) return s_playback == Playback::Idle;
	auto* const log = BattleLogEvent();
	if (!log) return false;
	const char* state = StateName(BattleLogState(log));
	// Versus and Battle run the replay; Select again means it is over.
	if (!std::strcmp(state, "Versus") || !std::strcmp(state, "Battle")) {
		// The Versus splash (the state's +0x48) waits for its movies and the
		// announcer, which do not finish here; after two seconds it gets the
		// Start press the player could give it (Dimps__Game.hxx, ReplayBattle).
		using Dimps::Game::ReplayBattle;
		static int ticks = 0, pressed = 0;
		if (s_playbackWait == 0) { ticks = 0; pressed = 0; }
		if (!std::strcmp(state, "Versus")) {
			++ticks;
			auto* const controller = (log->*Dimps::Event::EventBaseWithEC::publicMethods.GetChildEventController)();
			auto* const versus = reinterpret_cast<std::uint8_t*>((controller->*Dimps::Event::EventController::publicMethods.GetForegroundEvent)());
			std::uint8_t* const splash = versus ? *reinterpret_cast<std::uint8_t**>(versus + 0x48) : nullptr;
			if (splash && ticks > 120 && !pressed && *reinterpret_cast<int*>(splash + ReplayBattle::SplashState) == 1) {
				pressed = 1;
				const auto& native = ReplayBattle::staticMethods;
				auto* const voice = *reinterpret_cast<ReplayBattle::Voice**>(splash + ReplayBattle::SplashVoice);
				*reinterpret_cast<int*>(splash + ReplayBattle::SplashPhase) = 3;
				if (voice) native.FadeVoice(voice, 0x1F);
				*reinterpret_cast<int*>(splash + ReplayBattle::SplashState) = 3;
				for (int movie = 0; movie < 2; movie++) {
					auto* const m = reinterpret_cast<ReplayBattle::Movie*>(splash + ReplayBattle::SplashMovies + movie * 8);
					if (native.MovieValid(m)) native.MovieSignal(m, "Close", 0);
				}
				spdlog::info("Replay: skipped the Versus splash");
			}
		}
		s_playbackWait = 1; return false;
	}
	if (!std::strcmp(state, "Select") && s_playbackWait) { s_playback = Playback::Idle; return true; }
	return false;
}

sf4e::replaystore::Outcome sf4e::replaystore::Import(const std::wstring& path, Playable* playable) {
	if (!s_entries) return Outcome::NotReady;
	platform::replays::Imported imported;
	if (!platform::replays::ImportFile(path, WriteThroughSteam, imported)) return Outcome::Failed;
	std::uint8_t* const entry = s_entries + imported.slot * kEntryBytes;
	Stream stream{nullptr, imported.record.data(), imported.record.data(), static_cast<std::uint32_t>(imported.record.size())};
	BOOL (Entry::* deserialize)(Stream*);
	*reinterpret_cast<PVOID*>(&deserialize) = (*reinterpret_cast<PVOID**>(entry))[2];
	if (!(reinterpret_cast<Entry*>(entry)->*deserialize)(&stream)) {
		// The files already hold the replay; the next game start lists it.
		spdlog::warn("Replay: the game did not take slot {}'s record; it shows after a restart", imported.slot);
		return Outcome::Failed;
	}
	std::memcpy(entry + kEntrySlotBytesOffset, imported.slotBytes.data(), 2);
	spdlog::info("Replay: slot {} is in the game's table", imported.slot);
	if (playable) { playable->slot = imported.slot; playable->replay = std::move(imported.replay); playable->record = std::move(imported.record); }
	return Outcome::Added;
}
