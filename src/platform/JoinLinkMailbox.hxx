#pragma once

// Hands a link from a second Launcher.exe (started by the browser for an
// ember: link) to the game that is already running: a room link
// (ember://join/...), a tournament match link (ember://tournament/open), a
// public room link (ember://room/open) or a Discord connect link
// (ember://discord/connect).
// The game holds, for each, a small named section, an auto-reset event and a
// mutex in this session's Local namespace; the launcher writes the slot and
// signals. All are created with the default security of the player's own
// token, so only the player's own processes can open them. The mutex covers a
// write with its signal and a read with its clear, so reading one link never
// erases a newer one. Whatever the game reads is checked again: any process of
// the player's could have written it.

#include <windows.h>

#include <cstring>
#include <string>

#include "../common/JoinLink.hxx"
#include "../common/TournamentLink.hxx"

namespace sf4e {
namespace platform {

inline const wchar_t* JoinLinkSectionName() { return L"Local\\SF4EmberJoinLink"; }
inline const wchar_t* JoinLinkEventName() { return L"Local\\SF4EmberJoinLinkReady"; }
inline const wchar_t* MatchLinkSectionName() { return L"Local\\SF4EmberMatchLink"; }
inline const wchar_t* MatchLinkEventName() { return L"Local\\SF4EmberMatchLinkReady"; }
inline const wchar_t* PublicRoomLinkSectionName() { return L"Local\\SF4EmberPublicRoomLink"; }
inline const wchar_t* PublicRoomLinkEventName() { return L"Local\\SF4EmberPublicRoomLinkReady"; }
inline const wchar_t* ConnectLinkSectionName() { return L"Local\\SF4EmberConnectLink"; }
inline const wchar_t* ReplayLinkSectionName() { return L"Local\\SF4EmberReplayLink"; }
inline const wchar_t* ReplayLinkEventName() { return L"Local\\SF4EmberReplayLinkReady"; }
inline const wchar_t* ConnectLinkEventName() { return L"Local\\SF4EmberConnectLinkReady"; }
inline std::wstring JoinLinkLockName(const wchar_t* section) { return std::wstring(section) + L"Lock"; }
inline bool JoinLinkLocked(DWORD wait) { return wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED; }

struct JoinLinkSlot {
	char code[16];
};

struct MatchLinkSlot {
	char bridge[48];
	char match[48];
};

struct PublicRoomLinkSlot {
	char bridge[48];
	char room[40];
};

struct ConnectLinkSlot {
	char bridge[48];
};

// Replay links: the file's path, UTF-8 (common/ReplayLink.hxx).
struct ReplayLinkSlot {
	char file[1024];
};

// The game's end of one slot. Open once; Take on the game thread, as often as wanted.
template <class Slot>
class SlotMailbox {
public:
	SlotMailbox(const wchar_t* section, const wchar_t* ready) : sectionName_(section), readyName_(ready) {}
	SlotMailbox(const SlotMailbox&) = delete;
	SlotMailbox& operator=(const SlotMailbox&) = delete;
	~SlotMailbox() { Close(); }

	bool Open() {
		if (slot_) return true;
		section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Slot), sectionName_.c_str());
		if (!section_) return false;
		slot_ = static_cast<Slot*>(MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Slot)));
		ready_ = CreateEventW(nullptr, FALSE, FALSE, readyName_.c_str());
		lock_ = CreateMutexW(nullptr, FALSE, JoinLinkLockName(sectionName_.c_str()).c_str());
		if (!slot_ || !ready_ || !lock_) { Close(); return false; }
		return true;
	}

	// Copies out and clears the slot written since the last call; false when
	// there is none. Never waits: while a launcher is writing, the slot is
	// left for the next call.
	bool Take(Slot& out) {
		SecureZeroMemory(&out, sizeof(out));
		if (!slot_ || !JoinLinkLocked(WaitForSingleObject(lock_, 0))) return false;
		bool taken = false;
		if (WaitForSingleObject(ready_, 0) == WAIT_OBJECT_0) {
			std::memcpy(&out, slot_, sizeof(Slot));
			SecureZeroMemory(slot_, sizeof(Slot));
			taken = true;
		}
		ReleaseMutex(lock_);
		return taken;
	}

private:
	void Close() {
		if (slot_) UnmapViewOfFile(slot_);
		if (section_) CloseHandle(section_);
		if (ready_) CloseHandle(ready_);
		if (lock_) CloseHandle(lock_);
		slot_ = nullptr; section_ = nullptr; ready_ = nullptr; lock_ = nullptr;
	}
	std::wstring sectionName_, readyName_;
	HANDLE section_ = nullptr;
	HANDLE ready_ = nullptr;
	HANDLE lock_ = nullptr;
	Slot* slot_ = nullptr;
};

// The launcher's end: false when no game is listening.
template <class Slot>
bool DeliverSlot(const Slot& value, const wchar_t* sectionName, const wchar_t* readyName) {
	HANDLE section = OpenFileMappingW(FILE_MAP_WRITE, FALSE, sectionName);
	if (!section) return false;
	auto* slot = static_cast<Slot*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(Slot)));
	HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
	HANDLE lock = OpenMutexW(SYNCHRONIZE, FALSE, JoinLinkLockName(sectionName).c_str());
	bool signalled = false;
	// The game holds the lock for a copy at most, so this wait is short.
	if (slot && ready && lock && JoinLinkLocked(WaitForSingleObject(lock, 2000))) {
		std::memcpy(slot, &value, sizeof(Slot));
		signalled = SetEvent(ready) != FALSE;
		ReleaseMutex(lock);
	}
	if (lock) CloseHandle(lock);
	if (ready) CloseHandle(ready);
	if (slot) UnmapViewOfFile(slot);
	CloseHandle(section);
	return signalled;
}

// Room links: the 12-symbol code.
class JoinLinkMailbox {
public:
	// Tests pass names of their own so they never meet a running game.
	JoinLinkMailbox(const wchar_t* section = JoinLinkSectionName(), const wchar_t* ready = JoinLinkEventName())
		: slots_(section, ready) {}
	bool Open() { return slots_.Open(); }
	// The newest code handed over since the last call, or "".
	std::string Take() {
		JoinLinkSlot slot;
		if (!slots_.Take(slot)) return std::string();
		char copy[sizeof(slot.code) + 1] = {};
		std::memcpy(copy, slot.code, sizeof(slot.code));
		SecureZeroMemory(&slot, sizeof(slot));
		return join_link::ParseCode(copy);
	}
private:
	SlotMailbox<JoinLinkSlot> slots_;
};

inline bool DeliverJoinLink(const std::string& code, const wchar_t* sectionName = JoinLinkSectionName(),
	const wchar_t* readyName = JoinLinkEventName()) {
	if (code.size() != join_link::CodeSymbols || join_link::ParseCode(code) != code) return false;
	JoinLinkSlot slot = {};
	std::memcpy(slot.code, code.data(), code.size());
	return DeliverSlot(slot, sectionName, readyName);
}

// Tournament match links: the bridge and the match.
class MatchLinkMailbox {
public:
	MatchLinkMailbox(const wchar_t* section = MatchLinkSectionName(), const wchar_t* ready = MatchLinkEventName())
		: slots_(section, ready) {}
	bool Open() { return slots_.Open(); }
	// The newest link since the last call, or an invalid one.
	tournament_link::MatchLink Take() {
		MatchLinkSlot slot;
		if (!slots_.Take(slot)) return tournament_link::MatchLink();
		char bridge[sizeof(slot.bridge) + 1] = {}, match[sizeof(slot.match) + 1] = {};
		std::memcpy(bridge, slot.bridge, sizeof(slot.bridge));
		std::memcpy(match, slot.match, sizeof(slot.match));
		return tournament_link::Checked(bridge, match);
	}
private:
	SlotMailbox<MatchLinkSlot> slots_;
};

inline bool DeliverMatchLink(const tournament_link::MatchLink& link,
	const wchar_t* sectionName = MatchLinkSectionName(), const wchar_t* readyName = MatchLinkEventName()) {
	if (!tournament_link::Checked(link.bridgeId, link.matchId).Valid()) return false;
	MatchLinkSlot slot = {};
	std::memcpy(slot.bridge, link.bridgeId.data(), link.bridgeId.size());
	std::memcpy(slot.match, link.matchId.data(), link.matchId.size());
	return DeliverSlot(slot, sectionName, readyName);
}

// Public room links: the bridge and the room.
class PublicRoomLinkMailbox {
public:
	PublicRoomLinkMailbox(const wchar_t* section = PublicRoomLinkSectionName(), const wchar_t* ready = PublicRoomLinkEventName())
		: slots_(section, ready) {}
	bool Open() { return slots_.Open(); }
	// The newest link since the last call, or an invalid one.
	tournament_link::RoomLink Take() {
		PublicRoomLinkSlot slot;
		if (!slots_.Take(slot)) return tournament_link::RoomLink();
		char bridge[sizeof(slot.bridge) + 1] = {}, room[sizeof(slot.room) + 1] = {};
		std::memcpy(bridge, slot.bridge, sizeof(slot.bridge));
		std::memcpy(room, slot.room, sizeof(slot.room));
		return tournament_link::CheckedRoom(bridge, room);
	}
private:
	SlotMailbox<PublicRoomLinkSlot> slots_;
};

inline bool DeliverPublicRoomLink(const tournament_link::RoomLink& link,
	const wchar_t* sectionName = PublicRoomLinkSectionName(), const wchar_t* readyName = PublicRoomLinkEventName()) {
	if (!tournament_link::CheckedRoom(link.bridgeId, link.roomId).Valid()) return false;
	PublicRoomLinkSlot slot = {};
	std::memcpy(slot.bridge, link.bridgeId.data(), link.bridgeId.size());
	std::memcpy(slot.room, link.roomId.data(), link.roomId.size());
	return DeliverSlot(slot, sectionName, readyName);
}

// Discord connect links: the service.
class ConnectLinkMailbox {
public:
	ConnectLinkMailbox(const wchar_t* section = ConnectLinkSectionName(), const wchar_t* ready = ConnectLinkEventName())
		: slots_(section, ready) {}
	bool Open() { return slots_.Open(); }
	// The newest service since the last call, or "".
	std::string Take() {
		ConnectLinkSlot slot;
		if (!slots_.Take(slot)) return std::string();
		char bridge[sizeof(slot.bridge) + 1] = {};
		std::memcpy(bridge, slot.bridge, sizeof(slot.bridge));
		return tournament_link::IsBridgeId(bridge) ? std::string(bridge) : std::string();
	}
private:
	SlotMailbox<ConnectLinkSlot> slots_;
};

// Replay links: the file to play.
class ReplayLinkMailbox {
public:
	ReplayLinkMailbox(const wchar_t* section = ReplayLinkSectionName(), const wchar_t* ready = ReplayLinkEventName())
		: slots_(section, ready) {}
	bool Open() { return slots_.Open(); }
	// The newest file since the last call, or "".
	std::string Take() {
		ReplayLinkSlot slot;
		if (!slots_.Take(slot)) return std::string();
		char file[sizeof(slot.file) + 1] = {};
		std::memcpy(file, slot.file, sizeof(slot.file));
		return std::string(file);
	}
private:
	SlotMailbox<ReplayLinkSlot> slots_;
};

inline bool DeliverReplayLink(const std::string& file,
	const wchar_t* sectionName = ReplayLinkSectionName(), const wchar_t* readyName = ReplayLinkEventName()) {
	ReplayLinkSlot slot = {};
	if (file.empty() || file.size() >= sizeof(slot.file)) return false;
	std::memcpy(slot.file, file.data(), file.size());
	return DeliverSlot(slot, sectionName, readyName);
}

inline bool DeliverConnectLink(const std::string& bridge,
	const wchar_t* sectionName = ConnectLinkSectionName(), const wchar_t* readyName = ConnectLinkEventName()) {
	if (!tournament_link::IsBridgeId(bridge)) return false;
	ConnectLinkSlot slot = {};
	std::memcpy(slot.bridge, bridge.data(), bridge.size());
	return DeliverSlot(slot, sectionName, readyName);
}

} // namespace platform
} // namespace sf4e
