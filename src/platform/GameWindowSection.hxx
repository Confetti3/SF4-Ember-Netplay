#pragma once

// Tells a second Launcher.exe (started by the browser for an ember: link) which
// window is the running game's, so that it can bring that window forward after
// it handed the link over (JoinLinkMailbox.hxx).
// The game publishes its window in a small named section in this session's
// Local namespace, created with the default security of the player's own token
// like the link slots. It is kept apart from them, since reading a link slot
// clears it. It needs no clearing: it lives as long as the game's process and
// is gone when the game exits. Whatever the launcher reads is checked again:
// any process of the player's could have written it.

#include <windows.h>

#include <string>

namespace sf4e {
namespace platform {

inline const wchar_t* GameWindowSectionName() { return L"Local\\SF4EmberGameWindow"; }

// A window handle holds 32 significant bits on 32-bit and 64-bit Windows
// alike, so the record is the same for the game and for any launcher.
struct GameWindowRecord {
	DWORD pid;
	DWORD window;
};

// The game's end. Publish on the game thread, as often as wanted.
class GameWindowPublisher {
public:
	// Tests pass a name of their own so they never meet a running game.
	explicit GameWindowPublisher(const wchar_t* name = GameWindowSectionName()) : name_(name) {}
	GameWindowPublisher(const GameWindowPublisher&) = delete;
	GameWindowPublisher& operator=(const GameWindowPublisher&) = delete;
	~GameWindowPublisher() {
		if (record_) UnmapViewOfFile(record_);
		if (section_) CloseHandle(section_);
	}

	// Cheap while the window is unchanged. The section is created once, at the
	// first window; if that fails the game just goes on unfocused by a launcher.
	void Publish(HWND window) {
		const DWORD value = HandleToUlong(window);
		if (value == published_) return;
		if (!tried_) { tried_ = true; Open(); }
		if (!record_) return;
		// One aligned write, so a launcher never reads half of a change.
		InterlockedExchange(reinterpret_cast<volatile LONG*>(&record_->window), static_cast<LONG>(value));
		published_ = value;
	}

private:
	void Open() {
		section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GameWindowRecord), name_.c_str());
		if (!section_) return;
		record_ = static_cast<GameWindowRecord*>(MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GameWindowRecord)));
		if (record_) record_->pid = GetCurrentProcessId();
	}
	std::wstring name_;
	HANDLE section_ = nullptr;
	GameWindowRecord* record_ = nullptr;
	DWORD published_ = 0;
	bool tried_ = false;
};

// The launcher's end: false when no game published a window.
inline bool ReadGameWindow(GameWindowRecord& out, const wchar_t* name = GameWindowSectionName()) {
	out = GameWindowRecord{};
	HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
	if (!section) return false;
	const auto* record = static_cast<const GameWindowRecord*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(GameWindowRecord)));
	if (record) {
		out = *record;
		UnmapViewOfFile(record);
	}
	CloseHandle(section);
	return out.pid != 0 && out.window != 0;
}

} // namespace platform
} // namespace sf4e
