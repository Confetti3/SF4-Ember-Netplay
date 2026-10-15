#pragma once

// A call back from Training or a ready challenge, told to a player whose game
// window is behind another or minimized (common/AwayAlert.hxx).
namespace sf4e {
	namespace AwayAlert {
		// Game thread, from the transition that decides the call or the
		// challenge, once for each, so it comes whether or not frames are
		// drawn. While the window is away the taskbar button flashes until
		// the window comes forward, and with sound on a chime plays through
		// Windows at volumePercent. Never waits on either.
		void Announce(bool sound, int volumePercent, const char* what);
	}
}
