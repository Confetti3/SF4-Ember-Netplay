#pragma once

#include <cstdint>

namespace sf4e {
namespace launcher {

	void FormatRoomCode(const char* host, uint16_t port, char* outCode, int outCodeLen);
	bool ParseRoomCode(const char* roomCode, char* outHost, int outHostLen, uint16_t* outPort);

	bool IsShortRoomCode(const char* roomCode);
	bool NormalizeRelayRoomCode(const char* roomCode, char* outCode, int outCodeLen);
	bool FormatShortRoomCode(const char* token, char* outCode, int outCodeLen);
	bool ParseShortRoomCode(const char* roomCode, char* outToken, int outTokenLen);

	bool LooksLikeHostPortAddress(const char* roomCode);

} // namespace launcher
} // namespace sf4e
