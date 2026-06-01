#pragma once

#include <windows.h>

#include "../common/sf4e__NetplayConfig.hxx"
#include "netplay_persist.hxx"

namespace sf4e {
namespace launcher {

	// Returns true to launch the game; false if user cancelled.
	bool RunNetplayWizard(HWND parent, NetplayConfig& outConfig, PersistedSettings& ioSettings);

	// CLI / headless path (--host, --join).
	bool ApplyNetplayConfigFromWizard(
		NetplayConfig& outConfig,
		PersistedSettings& ioSettings,
		int mode,
		const char* joinRoomCodeOrNull
	);

} // namespace launcher
} // namespace sf4e
