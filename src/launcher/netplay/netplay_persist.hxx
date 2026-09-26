#pragma once

#include "../common/sf4e__NetplayConfig.hxx"

#include <string>

namespace sf4e {
namespace launcher {

	struct PersistedSettings {
		char displayName[NETPLAY_DISPLAY_NAME_LEN] = "Player";
		uint8_t inputDelay = 2;
		uint8_t editionSelect = 1;
		int roundCount = 3;
		int roundTimeIntegral = 99;
		// The game folder picked in launch recovery, as UTF-8. Empty until one is picked.
		std::string gameDirectory;
	};

	bool LoadPersistedSettings(PersistedSettings& out);
	bool SavePersistedSettings(const PersistedSettings& in);
	// Saves only the game folder, so preferences the game changed after this
	// launcher loaded them are not overwritten.
	bool SaveGameDirectory(const std::string& gameDirectory);
	// Replaces an empty or default "Player" name with a unique one and saves it.
	void EnsureUniqueDisplayName(PersistedSettings& settings);
	bool GetConfigFilePath(wchar_t* outPath, int outPathChars);

} // namespace launcher
} // namespace sf4e
