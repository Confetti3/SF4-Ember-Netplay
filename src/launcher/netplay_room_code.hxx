#pragma once

#include <cstdint>

namespace sf4e {
namespace launcher {

	void FormatRoomCode(const char* host, uint16_t port, char* outCode, int outCodeLen);
	bool ParseRoomCode(const char* roomCode, char* outHost, int outHostLen, uint16_t* outPort);

	bool IsShortRoomCode(const char* roomCode);
	bool FormatShortRoomCode(const char* token, char* outCode, int outCodeLen);
	bool ParseShortRoomCode(const char* roomCode, char* outToken, int outTokenLen);

} // namespace launcher
} // namespace sf4e
