#pragma once

// Hands a room link from a second Launcher.exe (started by the browser for
// ember://join/...) to the game that is already running. The game holds a
// small named section and an auto-reset event in this session's Local
// namespace; the launcher writes the 12-symbol code and signals. Both are
// created with the default security of the player's own token, so only the
// player's own processes can open them, and the game only ever shows the code
// on its Join screen for the player to confirm.

#include <windows.h>

#include <cstring>
#include <string>

#include "../common/JoinLink.hxx"

namespace sf4e {
namespace platform {

inline const wchar_t* JoinLinkSectionName() { return L"Local\\SF4EmberJoinLink"; }
inline const wchar_t* JoinLinkEventName() { return L"Local\\SF4EmberJoinLinkReady"; }

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
		if (!slot_ || !ready_) { Close(); return false; }
		return true;
	}

	// The newest code handed over since the last call, or "". Never waits.
	std::string Take() {
		if (!slot_ || WaitForSingleObject(ready_, 0) != WAIT_OBJECT_0) return std::string();
		char copy[sizeof(JoinLinkSlot::code) + 1] = {};
		std::memcpy(copy, slot_->code, sizeof(JoinLinkSlot::code));
		SecureZeroMemory(slot_->code, sizeof(JoinLinkSlot::code));
		return join_link::ParseCode(copy);
	}

private:
	void Close() {
		if (slot_) UnmapViewOfFile(slot_);
		if (section_) CloseHandle(section_);
		if (ready_) CloseHandle(ready_);
		slot_ = nullptr; section_ = nullptr; ready_ = nullptr;
	}
	std::wstring sectionName_, readyName_;
	HANDLE section_ = nullptr;
	HANDLE ready_ = nullptr;
	JoinLinkSlot* slot_ = nullptr;
};

// The launcher's end: false when no game is listening.
inline bool DeliverJoinLink(const std::string& code, const wchar_t* sectionName = JoinLinkSectionName(),
	const wchar_t* readyName = JoinLinkEventName()) {
	if (code.size() != join_link::CodeSymbols || join_link::ParseCode(code) != code) return false;
	HANDLE section = OpenFileMappingW(FILE_MAP_WRITE, FALSE, sectionName);
	if (!section) return false;
	auto* slot = static_cast<JoinLinkSlot*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(JoinLinkSlot)));
	if (!slot) { CloseHandle(section); return false; }
	SecureZeroMemory(slot->code, sizeof(slot->code));
	std::memcpy(slot->code, code.data(), code.size());
	UnmapViewOfFile(slot);
	CloseHandle(section);
	HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
	if (!ready) return false;
	const bool signalled = SetEvent(ready) != FALSE;
	CloseHandle(ready);
	return signalled;
}

} // namespace platform
} // namespace sf4e
