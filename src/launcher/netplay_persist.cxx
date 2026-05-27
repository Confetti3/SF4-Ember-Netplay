#include "netplay_persist.hxx"

#include <fstream>
#include <shlobj.h>
#include <pathcch.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace sf4e {
namespace launcher {

	namespace {

		const char* kDefaultBrokerBaseUrl = "http://74.208.200.95:8787";

		bool BrokerUrlNeedsMigration(const char* brokerBaseUrl) {
			if (!brokerBaseUrl || !brokerBaseUrl[0]) {
				return false;
			}
			return _stricmp(brokerBaseUrl, "http://150.136.121.155:8787") == 0
				|| _stricmp(brokerBaseUrl, "http://150.136.121.155:8787/") == 0;
		}

		void ClearSessionRelayFields(PersistedSettings& settings) {
			settings.relayRoomCode[0] = '\0';
			settings.relayHostSecret[0] = '\0';
			settings.relaySessionPort = 0;
		}

		void MigrateDeprecatedBrokerUrl(PersistedSettings& settings) {
			if (!BrokerUrlNeedsMigration(settings.brokerBaseUrl)) {
				return;
			}
			strncpy_s(settings.brokerBaseUrl, kDefaultBrokerBaseUrl, _TRUNCATE);
			ClearSessionRelayFields(settings);
		}

	} // namespace

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
		wchar_t path[MAX_PATH] = { 0 };
		if (!GetConfigFilePath(path, MAX_PATH)) {
			return false;
		}

		std::ifstream f(path);
		if (!f.is_open()) {
			return false;
		}

		try {
			nlohmann::json j;
			f >> j;
			std::string name = j.value("displayName", "Player");
			strncpy_s(out.displayName, name.c_str(), _TRUNCATE);
			out.inputDelay = (uint8_t)j.value("inputDelay", 2);
			out.sessionPort = (uint16_t)j.value("sessionPort", 23456);
			out.ggpoPort = (uint16_t)j.value("ggpoPort", 23457);
			out.editionSelect = (uint8_t)j.value("editionSelect", 1);
			out.roundCount = j.value("roundCount", 3);
			out.roundTimeIntegral = j.value("roundTimeIntegral", 99);
			out.useRelay = (uint8_t)j.value("useRelay", 1);
			std::string lastJoin = j.value("lastJoinHost", "");
			strncpy_s(out.lastJoinHost, lastJoin.c_str(), _TRUNCATE);
			std::string lastAdv = j.value("lastAdvertiseHost", "");
			strncpy_s(out.lastAdvertiseHost, lastAdv.c_str(), _TRUNCATE);
			out.simpleUi = (uint8_t)j.value("simpleUi", 1);
			out.defaultConnectMethod = (uint8_t)j.value("defaultConnectMethod", 1);
			std::string broker = j.value("brokerBaseUrl", kDefaultBrokerBaseUrl);
			strncpy_s(out.brokerBaseUrl, broker.c_str(), _TRUNCATE);
			const bool hadStoredRelay =
				(j.contains("relayRoomCode") && !j.value("relayRoomCode", "").empty())
				|| (j.contains("relayHostSecret") && !j.value("relayHostSecret", "").empty())
				|| j.value("relaySessionPort", 0) != 0;
			ClearSessionRelayFields(out);
			const bool brokerMigrated = BrokerUrlNeedsMigration(out.brokerBaseUrl);
			MigrateDeprecatedBrokerUrl(out);
			if (brokerMigrated || hadStoredRelay) {
				SavePersistedSettings(out);
			}
			return true;
		}
		catch (...) {
			spdlog::warn("Could not parse launcher config.json");
			return false;
		}
	}

	bool SavePersistedSettings(const PersistedSettings& in) {
		wchar_t path[MAX_PATH] = { 0 };
		if (!GetConfigFilePath(path, MAX_PATH)) {
			return false;
		}

		nlohmann::json j;
		j["displayName"] = in.displayName;
		j["inputDelay"] = in.inputDelay;
		j["sessionPort"] = in.sessionPort;
		j["ggpoPort"] = in.ggpoPort;
		j["editionSelect"] = in.editionSelect;
		j["roundCount"] = in.roundCount;
		j["roundTimeIntegral"] = in.roundTimeIntegral;
		j["useRelay"] = in.useRelay;
		j["lastJoinHost"] = in.lastJoinHost;
		j["lastAdvertiseHost"] = in.lastAdvertiseHost;
		j["simpleUi"] = in.simpleUi;
		j["defaultConnectMethod"] = in.defaultConnectMethod;
		j["brokerBaseUrl"] = in.brokerBaseUrl;

		std::ofstream f(path);
		if (!f.is_open()) {
			return false;
		}
		f << j.dump(2);
		return true;
	}

} // namespace launcher
} // namespace sf4e
