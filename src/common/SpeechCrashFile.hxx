#pragma once

// The start-up half of the speech-crash marker (SpeechCrash.hxx): read the
// marker the crash handler left in a logs folder, turn speech off in the saved
// preferences, and only then remove it. The folder and the save are passed in,
// so the test uses a scratch folder and no real marker is ever touched.
//
// Pure component: no Windows, game or helper dependencies, unit tested.

#include "SpeechCrash.hxx"

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>

namespace sf4e {
namespace crash {

struct SpeechCrashRecovery {
	// A marker was there: the engine is not to be started this session.
	bool found = false;
	// Speech was on and is now off: the menu says so once.
	bool turnedOff = false;
	// The faulting DLL, for the log.
	std::string module;
};

// `enabled` is the loaded speech setting; `saveOff` saves it as off and says
// whether that worked. The marker is removed once the player's setting no
// longer needs it: speech already off, or the off saved. When the save fails
// it stays, so the next start tries again. A file that is no marker (empty or
// foreign) is removed and changes nothing.
inline SpeechCrashRecovery RecoverFromSpeechCrash(const std::filesystem::path& logsDirectory, bool& enabled, const std::function<bool()>& saveOff) {
	SpeechCrashRecovery recovery;
	const std::filesystem::path marker = logsDirectory / SpeechCrashMarkerFile;
	std::error_code ignored;
	if (!std::filesystem::exists(marker, ignored)) return recovery;
	char text[256] = {};
	size_t length = 0;
	{
		std::ifstream file(marker, std::ios::binary);
		if (file) { file.read(text, sizeof(text) - 1); length = static_cast<size_t>(file.gcount()); }
	}
	recovery.found = ParseSpeechCrashMarker(text, length, &recovery.module);
	if (recovery.found && enabled) {
		enabled = false;
		recovery.turnedOff = true;
		if (!saveOff()) return recovery;
	}
	std::filesystem::remove(marker, ignored);
	return recovery;
}

} // namespace crash
} // namespace sf4e
