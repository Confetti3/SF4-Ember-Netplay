#include "netplay_persist.hxx"
#include "../../netplay/SettingsStore.hxx"

#include <fstream>
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
			out.inputDelay = static_cast<uint8_t>(delay >= 0 && delay <= 10 ? delay : 2);
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
