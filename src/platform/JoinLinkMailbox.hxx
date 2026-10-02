#pragma once

// Hands a link from a second Launcher.exe (started by the browser for an
// ember: link) to the game that is already running: a room link
// (ember://join/...) or a tournament match handoff (ember://tournament/open).
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
inline const wchar_t* TournamentHandoffSectionName() { return L"Local\\SF4EmberTournamentHandoff"; }
inline const wchar_t* TournamentHandoffEventName() { return L"Local\\SF4EmberTournamentHandoffReady"; }
inline std::wstring JoinLinkLockName(const wchar_t* section) { return std::wstring(section) + L"Lock"; }
inline bool JoinLinkLocked(DWORD wait) { return wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED; }

struct JoinLinkSlot {
	char code[16];
};

struct TournamentHandoffSlot {
	char bridge[48];
	char code[48];
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

// Tournament handoffs: the bridge and the one-use code.
class TournamentHandoffMailbox {
public:
	TournamentHandoffMailbox(const wchar_t* section = TournamentHandoffSectionName(),
		const wchar_t* ready = TournamentHandoffEventName()) : slots_(section, ready) {}
	bool Open() { return slots_.Open(); }
	// The newest handoff since the last call, or an invalid one.
	tournament_link::Handoff Take() {
		TournamentHandoffSlot slot;
		tournament_link::Handoff handoff;
		if (!slots_.Take(slot)) return handoff;
		char bridge[sizeof(slot.bridge) + 1] = {}, code[sizeof(slot.code) + 1] = {};
		std::memcpy(bridge, slot.bridge, sizeof(slot.bridge));
		std::memcpy(code, slot.code, sizeof(slot.code));
		SecureZeroMemory(&slot, sizeof(slot));
		if (tournament_link::IsBridgeId(bridge) && tournament_link::IsHandoffCode(code)) {
			handoff.bridgeId = bridge;
			handoff.code = code;
		}
		SecureZeroMemory(code, sizeof(code));
		return handoff;
	}
private:
	SlotMailbox<TournamentHandoffSlot> slots_;
};

inline bool DeliverTournamentHandoff(const tournament_link::Handoff& handoff,
	const wchar_t* sectionName = TournamentHandoffSectionName(), const wchar_t* readyName = TournamentHandoffEventName()) {
	if (!tournament_link::IsBridgeId(handoff.bridgeId) || !tournament_link::IsHandoffCode(handoff.code)) return false;
	TournamentHandoffSlot slot = {};
	std::memcpy(slot.bridge, handoff.bridgeId.data(), handoff.bridgeId.size());
	std::memcpy(slot.code, handoff.code.data(), handoff.code.size());
	const bool delivered = DeliverSlot(slot, sectionName, readyName);
	SecureZeroMemory(&slot, sizeof(slot));
	return delivered;
}

} // namespace platform
} // namespace sf4e
