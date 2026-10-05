#pragma once

// Shared by PublicRoomHostTest and PublicRoomSoakTest: the constants a room host
// and a ticket must agree on, and the room_ticket example runner. A room host
// elsewhere (PublicRoomHostTest --remote, server/roomhost/soak/soak-hosts.sh) is
// configured with these same values. Include after the session headers, which
// pull in winsock2.h before windows.h.
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

#include "test_support.hxx"

namespace sf4e { namespace test { namespace publicroom {

inline const char* const Build = "public-room-host-test";
inline const char* const Bridge = "brg_6f1c0d2a-6a9c-4f30-9c5e-0d8f4f0b9a11";
inline const char* const RoomIdHex = "5f1e0d3c2b4a69788796a5b4c3d2e1f0";

inline std::string Utf8(const std::wstring& text) {
	if (text.empty()) return {};
	const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<std::size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], length, nullptr, nullptr);
	return out;
}

// The ticket tool's path, set once at startup.
inline std::wstring ticketTool;

// Runs the ticket tool and collects its trimmed stdout lines. False when the
// tool could not be started or exited non-zero.
inline bool RunTool(const std::string& arguments, std::vector<std::string>& lines) {
	lines.clear();
	const std::wstring command = L"\"\"" + ticketTool + L"\" " + std::wstring(arguments.begin(), arguments.end()) + L"\"";
	FILE* pipe = _wpopen(command.c_str(), L"rt");
	if (!pipe) return false;
	char buffer[2048];
	while (std::fgets(buffer, sizeof(buffer), pipe)) {
		std::string line(buffer);
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
		if (!line.empty()) lines.push_back(line);
	}
	return _pclose(pipe) == 0;
}

// The same, ending the process when the tool fails (the fixtures' use).
inline std::vector<std::string> Tool(const std::string& arguments) {
	std::vector<std::string> lines;
	CHECK(RunTool(arguments, lines));
	return lines;
}

inline std::string Seed(char digit) { return std::string(64, digit); }

} } }
