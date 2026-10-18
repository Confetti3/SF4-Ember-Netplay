#pragma once

// After a second launcher handed an ember: link to the running game, the
// player is still looking at the browser that started it. This brings the
// game's window forward so the match, room or replay can be seen.

#include <windows.h>

#include <spdlog/spdlog.h>

#include "../platform/GameWindowSection.hxx"

namespace sf4e {
namespace launcher {

// The browser that took the click started this process, which is why Windows
// lets it change the foreground window. When it does not, the taskbar button flashes.
inline void FocusRunningGame() {
	platform::GameWindowRecord game;
	if (!platform::ReadGameWindow(game)) {
		spdlog::info("No game window was published; nothing to bring forward");
		return;
	}
	// The record was written by the player's own process, but its window may be
	// gone since, and its number reused: only a window of that process is touched.
	const HWND window = static_cast<HWND>(UlongToHandle(game.window));
	DWORD owner = 0;
	if (!IsWindow(window) || !GetWindowThreadProcessId(window, &owner) || owner != game.pid) {
		spdlog::info("The published game window is gone; nothing to bring forward");
		return;
	}
	AllowSetForegroundWindow(game.pid);
	if (IsIconic(window)) {
		// What the taskbar sends to a minimized window. The game restores
		// itself from its device reset as it does after an alt-tab. The wait
		// is bounded: a game that does not answer must not hold the launcher.
		DWORD_PTR ignored = 0;
		SendMessageTimeoutW(window, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 1000, &ignored);
	}
	SetForegroundWindow(window);
	if (GetForegroundWindow() == window) {
		spdlog::info("Brought the running game's window forward");
		return;
	}
	FLASHWINFO flash{ sizeof(flash), window, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0 };
	FlashWindowEx(&flash);
	spdlog::info("Windows kept another window in front; flashed the running game's taskbar button");
}

} // namespace launcher
} // namespace sf4e
