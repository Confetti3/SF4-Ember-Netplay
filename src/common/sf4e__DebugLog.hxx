#pragma once

#include <cstdio>
#include <ctime>
#include <shlobj.h>
#include <pathcch.h>

// #region agent log
namespace sf4e {
namespace debug {

	inline void AgentLog(const char* hypothesisId, const char* location, const char* message, const char* dataJson = "{}") {
		wchar_t logPath[MAX_PATH] = { 0 };
		const char* envPath = getenv("SF4E_DEBUG_LOG");
		FILE* f = nullptr;
		if (envPath && envPath[0]) {
			fopen_s(&f, envPath, "a");
		}
		else {
			wchar_t appData[MAX_PATH] = { 0 };
			if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appData))) {
				wchar_t sf4eDir[MAX_PATH] = { 0 };
				if (SUCCEEDED(PathCchCombine(sf4eDir, MAX_PATH, appData, L"sf4e"))) {
					CreateDirectoryW(sf4eDir, nullptr);
					if (SUCCEEDED(PathCchCombine(logPath, MAX_PATH, sf4eDir, L"debug-56757f.log"))) {
						_wfopen_s(&f, logPath, L"a");
					}
				}
			}
		}
		if (!f) {
			return;
		}
		fprintf(
			f,
			"{\"sessionId\":\"56757f\",\"hypothesisId\":\"%s\",\"location\":\"%s\",\"message\":\"%s\",\"data\":%s,\"timestamp\":%lld}\n",
			hypothesisId ? hypothesisId : "",
			location ? location : "",
			message ? message : "",
			dataJson && dataJson[0] ? dataJson : "{}",
			(long long)time(nullptr) * 1000
		);
		fflush(f);
		fclose(f);
	}

} // namespace debug
} // namespace sf4e
// #endregion
