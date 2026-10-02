#pragma once

// Hands a room link from a second Launcher.exe (started by the browser for
// ember://join/...) to the game that is already running. The game holds a
// small named section, an auto-reset event and a mutex in this session's
// Local namespace; the launcher writes the 12-symbol code and signals. All
// are created with the default security of the player's own token, so only
// the player's own processes can open them. The mutex covers a write with
// its signal and a read with its clear, so reading one link never erases a
// newer one.

#include <windows.h>

#include <cstring>
#include <string>

#include "../common/JoinLink.hxx"

namespace sf4e {
namespace platform {

inline const wchar_t* JoinLinkSectionName() { return L"Local\\SF4EmberJoinLink"; }
inline const wchar_t* JoinLinkEventName() { return L"Local\\SF4EmberJoinLinkReady"; }
inline std::wstring JoinLinkLockName(const wchar_t* section) { return std::wstring(section) + L"Lock"; }
inline bool JoinLinkLocked(DWORD wait) { return wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED; }

struct JoinLinkSlot {
	char code[16];
};

// The game's end. Open once; Take on the game thread, as often as wanted.
class JoinLinkMailbox {
public:
	// Tests pass names of their own so they never meet a running game.
	JoinLinkMailbox(const wchar_t* section = JoinLinkSectionName(), const wchar_t* ready = JoinLinkEventName())
		: sectionName_(section), readyName_(ready) {}
	JoinLinkMailbox(const JoinLinkMailbox&) = delete;
	JoinLinkMailbox& operator=(const JoinLinkMailbox&) = delete;
	~JoinLinkMailbox() { Close(); }

	bool Open() {
		if (slot_) return true;
		section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(JoinLinkSlot), sectionName_.c_str());
		if (!section_) return false;
		slot_ = static_cast<JoinLinkSlot*>(MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(JoinLinkSlot)));
		ready_ = CreateEventW(nullptr, FALSE, FALSE, readyName_.c_str());
		lock_ = CreateMutexW(nullptr, FALSE, JoinLinkLockName(sectionName_.c_str()).c_str());
		if (!slot_ || !ready_ || !lock_) { Close(); return false; }
		return true;
	}

	// The newest code handed over since the last call, or "". Never waits:
	// while a launcher is writing, the link is left for the next call.
	std::string Take() {
		if (!slot_ || !JoinLinkLocked(WaitForSingleObject(lock_, 0))) return std::string();
		char copy[sizeof(JoinLinkSlot::code) + 1] = {};
		if (WaitForSingleObject(ready_, 0) == WAIT_OBJECT_0) {
			std::memcpy(copy, slot_->code, sizeof(JoinLinkSlot::code));
			SecureZeroMemory(slot_->code, sizeof(JoinLinkSlot::code));
		}
		ReleaseMutex(lock_);
		return join_link::ParseCode(copy);
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
	JoinLinkSlot* slot_ = nullptr;
};

// The launcher's end: false when no game is listening.
inline bool DeliverJoinLink(const std::string& code, const wchar_t* sectionName = JoinLinkSectionName(),
	const wchar_t* readyName = JoinLinkEventName()) {
	if (code.size() != join_link::CodeSymbols || join_link::ParseCode(code) != code) return false;
	HANDLE section = OpenFileMappingW(FILE_MAP_WRITE, FALSE, sectionName);
	if (!section) return false;
	auto* slot = static_cast<JoinLinkSlot*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(JoinLinkSlot)));
	HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
	HANDLE lock = OpenMutexW(SYNCHRONIZE, FALSE, JoinLinkLockName(sectionName).c_str());
	bool signalled = false;
	// The game holds the lock for a copy at most, so this wait is short.
	if (slot && ready && lock && JoinLinkLocked(WaitForSingleObject(lock, 2000))) {
		SecureZeroMemory(slot->code, sizeof(slot->code));
		std::memcpy(slot->code, code.data(), code.size());
		signalled = SetEvent(ready) != FALSE;
		ReleaseMutex(lock);
	}
	if (lock) CloseHandle(lock);
	if (ready) CloseHandle(ready);
	if (slot) UnmapViewOfFile(slot);
	CloseHandle(section);
	return signalled;
}

} // namespace platform
} // namespace sf4e
