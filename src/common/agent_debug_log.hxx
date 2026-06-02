#pragma once

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>

#include <windows.h>

#include <nlohmann/json.hpp>

namespace sf4e {
namespace agent_debug {

inline bool Enabled() {
	char flag[8] = { 0 };
	return GetEnvironmentVariableA("SF4E_AGENT_DEBUG", flag, sizeof(flag)) > 0 && flag[0] == '1';
}

inline std::string LogPath() {
	char appdata[MAX_PATH] = { 0 };
	if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
		return std::string(appdata) + "\\sf4e\\debug-592d59.log";
	}
	return "debug-592d59.log";
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
		std::ofstream out(LogPath(), std::ios::app);
		if (!out) {
			return;
		}
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
		out << row.dump() << "\n";
	}
	catch (...) {
	}
	// #endregion
}

} // namespace agent_debug
} // namespace sf4e
