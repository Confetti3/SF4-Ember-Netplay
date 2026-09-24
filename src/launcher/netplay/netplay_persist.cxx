#include "netplay_persist.hxx"
#include "../../netplay/SettingsStore.hxx"
#include "../../common/InputDelay.hxx"

#include <fstream>
#include <random>
#include <string>
#include <shlobj.h>
#include <pathcch.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace sf4e {
namespace launcher {

	bool GetConfigFilePath(wchar_t* outPath, int outPathChars) {
		wchar_t appData[MAX_PATH] = { 0 };
		if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appData))) {
			return false;
		}
		if (FAILED(PathCchCombine(outPath, outPathChars, appData, L"sf4e"))) {
			return false;
		}
		CreateDirectoryW(outPath, NULL);
		if (FAILED(PathCchCombine(outPath, outPathChars, outPath, L"config.json"))) {
			return false;
		}
		return true;
	}

	bool LoadPersistedSettings(PersistedSettings& out) {
		nlohmann::json j;
		std::string error;
		netplay::SettingsStore store(netplay::SettingsStore::DefaultDirectory());
		if (!store.LoadLauncher(j, error)) {
			spdlog::warn("{}", error);
			return false;
		}

		try {
			std::string name = j.value("displayName", "Player");
			strncpy_s(out.displayName, name.c_str(), _TRUNCATE);
			const int delay = j.value("inputDelay", 2);
			out.inputDelay = static_cast<uint8_t>(delay >= 0 && delay <= MaximumInputDelay ? delay : 2);
			out.editionSelect = (uint8_t)j.value("editionSelect", 1);
			out.roundCount = j.value("roundCount", 3);
			out.roundTimeIntegral = j.value("roundTimeIntegral", 99);
			return true;
		}
		catch (...) {
			spdlog::warn("Could not parse launcher preferences in settings.json");
			return false;
		}
	}

	void EnsureUniqueDisplayName(PersistedSettings& settings) {
		// Rooms reject duplicate names, and every install used to default to
		// "Player", so two new players could never share a room. A saved
		// "Player" cannot be told apart from that default, so it is replaced too.
		if (settings.displayName[0] && strcmp(settings.displayName, "Player") != 0) return;
		std::random_device random;
		const std::string name = "Player " + std::to_string(std::uniform_int_distribution<int>(1000, 9999)(random));
		strncpy_s(settings.displayName, name.c_str(), _TRUNCATE);
		// Unsaved, the name still differs from other players for this launch.
		std::string error;
		if (!netplay::SettingsStore(netplay::SettingsStore::DefaultDirectory()).SaveLauncher({{"displayName", name}}, error))
			spdlog::warn("{}", error);
	}

	bool SavePersistedSettings(const PersistedSettings& in) {
		nlohmann::json j;
		j["displayName"] = in.displayName;
		j["inputDelay"] = in.inputDelay;
		j["editionSelect"] = in.editionSelect;
		j["roundCount"] = in.roundCount;
		j["roundTimeIntegral"] = in.roundTimeIntegral;

		std::string error;
		netplay::SettingsStore store(netplay::SettingsStore::DefaultDirectory());
		const bool saved = store.SaveLauncher(j, error);
		if (!saved) spdlog::warn("{}", error);
		return saved;
	}

} // namespace launcher
} // namespace sf4e
