#include <windows.h>
#include <mmsystem.h>
#include <vector>

#include "spdlog/spdlog.h"

#include "../common/AwayAlert.hxx"
#include "sf4e__AwayAlert.hxx"
#include "sf4e__BackgroundPlay.hxx"

namespace {
// One chime for each volume step, built on first use and kept: PlaySoundW
// reads an asynchronous sound from this memory while it plays.
std::vector<std::uint8_t> chimes[10];
}

void sf4e::AwayAlert::Announce(bool sound, int volumePercent, const char* what) {
	const HWND window = BackgroundPlay::GameWindow();
	if (!window) return;
	const bool minimized = IsIconic(window) != FALSE;
	const auto plan = away_alert::PlanFor(GetForegroundWindow() == window, minimized, sound);
	if (!plan.flash) return;
	FLASHWINFO flash{ sizeof(flash), window, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
	FlashWindowEx(&flash);
	bool played = false;
	if (plan.chime) {
		const int step = (volumePercent < 10 ? 10 : volumePercent > 100 ? 100 : volumePercent) / 10 - 1;
		auto& chime = chimes[step];
		if (chime.empty()) chime = away_alert::Chime((step + 1) * 10);
		// SND_ASYNC returns at once; SND_NODEFAULT keeps Windows' own sound out of a failure.
		played = PlaySoundW(reinterpret_cast<LPCWSTR>(chime.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT) != FALSE;
	}
	spdlog::info("Away alert: {} while the window was {}; taskbar flashed, chime {}", what,
		minimized ? "minimized" : "behind another", !plan.chime ? "off" : played ? "played" : "not played");
}
