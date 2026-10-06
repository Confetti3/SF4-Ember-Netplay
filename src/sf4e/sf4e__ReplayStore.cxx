#include "sf4e__ReplayStore.hxx"

#include <cstdint>
#include <cstring>
#include <string>
#include <windows.h>
#include <detours/detours.h>
#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game.hxx"
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

sf4e::replaystore::Outcome sf4e::replaystore::Import(const std::wstring& path) {
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
	return Outcome::Added;
}
