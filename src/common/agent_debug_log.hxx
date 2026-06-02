#pragma once

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

namespace sf4e {
namespace agent_debug {

inline bool Enabled() {
	char flag[8] = { 0 };
	if (GetEnvironmentVariableA("SF4E_AGENT_DEBUG", flag, sizeof(flag)) > 0 && flag[0] == '1') {
		return true;
	}
	char session[16] = { 0 };
	return GetEnvironmentVariableA("SF4E_DEBUG_SESSION", session, sizeof(session)) > 0 && session[0];
}

inline std::vector<std::string> LogPaths() {
	std::vector<std::string> paths;
	char custom[MAX_PATH] = { 0 };
	if (GetEnvironmentVariableA("SF4E_DEBUG_LOG", custom, MAX_PATH) > 0 && custom[0]) {
		paths.emplace_back(custom);
	}
	char appdata[MAX_PATH] = { 0 };
	if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
		paths.emplace_back(std::string(appdata) + "\\sf4e\\debug-592d59.log");
	}
	paths.emplace_back("debug-592d59.log");
	return paths;
}

inline void Log(
	const char* hypothesisId,
	const char* location,
	const char* message,
	const nlohmann::json& data = nlohmann::json::object()
) {
	if (!Enabled()) {
		return;
	}
	// #region agent log
	try {
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()
		).count();
		nlohmann::json row;
		row["sessionId"] = "592d59";
		row["hypothesisId"] = hypothesisId;
		row["location"] = location;
		row["message"] = message;
		row["data"] = data;
		row["timestamp"] = ms;
		const std::string line = row.dump() + "\n";
		for (const std::string& path : LogPaths()) {
			std::ofstream out(path, std::ios::app);
			if (out) {
				out << line;
			}
		}
	}
	catch (...) {
	}
	// #endregion
}

} // namespace agent_debug
} // namespace sf4e
