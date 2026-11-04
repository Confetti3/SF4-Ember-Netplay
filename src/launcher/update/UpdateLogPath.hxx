#pragma once
// Where the update log is on this PC, for the messages that send a player to
// it. Their text names it as %TEMP%\sf4-netplay-update.log in every language;
// that is not a path a player can open, least of all under Proton, where it
// lies inside the game's Wine prefix.
#include <Windows.h>
#include <string>
#include "../../common/ProductIdentity.hxx"

namespace sf4e { namespace launcher {

inline std::string UpdateLogUtf8(const std::wstring& text) {
	if (text.empty()) return std::string();
	const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string utf8(size > 0 ? size : 0, '\0');
	if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &utf8[0], size, nullptr, nullptr);
	return utf8;
}

// The update log the launcher and the updater write; empty where Windows gives
// no temporary folder.
inline std::wstring UpdateLogPathW() {
	wchar_t temp[MAX_PATH + 1] = {};
	const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
	if (length == 0 || length > MAX_PATH) return std::wstring();
	return std::wstring(temp, length) + ember::Product().updateLog;
}

// The log's full path, and under Wine the Linux path to the same file; empty
// where Windows gives no temporary folder.
inline std::string UpdateLogLocation() {
	const std::wstring path = UpdateLogPathW();
	if (path.empty()) return std::string();
	std::string location = UpdateLogUtf8(path);
	// wine_get_unix_file_name is cdecl and its result is freed from the process heap.
	using UnixName = char* (__cdecl*)(const wchar_t*);
	if (const auto unixName = reinterpret_cast<UnixName>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name"))) {
		if (char* name = unixName(path.c_str())) {
			if (*name) location += std::string(" (Linux: ") + name + ")";
			HeapFree(GetProcessHeap(), 0, name);
		}
	}
	return location;
}

// `text` with the log's placeholder name replaced by where it really is.
inline std::string WithUpdateLogLocation(std::string text) {
	static const std::string placeholder = "%TEMP%\\sf4-netplay-update.log";
	const std::string location = UpdateLogLocation();
	if (location.empty()) return text;
	for (std::size_t at = text.find(placeholder); at != std::string::npos; at = text.find(placeholder, at + location.size()))
		text.replace(at, placeholder.size(), location);
	return text;
}

} }
