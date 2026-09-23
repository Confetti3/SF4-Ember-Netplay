#pragma once

#include "../common/sf4e__NetplayConfig.hxx"

namespace sf4e {
namespace launcher {

	struct PersistedSettings {
		char displayName[NETPLAY_DISPLAY_NAME_LEN] = "Player";
		uint8_t inputDelay = 2;
		uint8_t editionSelect = 1;
		int roundCount = 3;
		int roundTimeIntegral = 99;
	};

	bool LoadPersistedSettings(PersistedSettings& out);
	bool SavePersistedSettings(const PersistedSettings& in);
	// Replaces an empty or default "Player" name with a unique one and saves it.
	void EnsureUniqueDisplayName(PersistedSettings& settings);
	bool GetConfigFilePath(wchar_t* outPath, int outPathChars);

} // namespace launcher
} // namespace sf4e
