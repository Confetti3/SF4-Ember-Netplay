#include "netplay_room_code.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/sf4e__NetUtil.hxx"

namespace sf4e {
namespace launcher {

	void FormatRoomCode(const char* host, uint16_t port, char* outCode, int outCodeLen) {
		snprintf(outCode, outCodeLen, "%s:%u", host, port);
	}

	bool ParseRoomCode(const char* roomCode, char* outHost, int outHostLen, uint16_t* outPort) {
		if (!roomCode || !outHost || !outPort) {
			return false;
		}
		char buf[256];
		strncpy_s(buf, roomCode, _TRUNCATE);
		sf4e::TrimRoomCodeInPlace(buf);
		if (!buf[0]) {
			return false;
		}

		char* colon = strrchr(buf, ':');
		if (!colon || colon == buf) {
			return false;
		}
		*colon = 0;
		const char* portStr = colon + 1;
		int port = atoi(portStr);
		if (port < 1 || port > 65535) {
			return false;
		}

		strncpy_s(outHost, outHostLen, buf, _TRUNCATE);
		if (!outHost[0]) {
			return false;
		}
		*outPort = (uint16_t)port;
		return true;
	}

	static bool IsAlnumCodeChar(char c) {
		return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
	}

	bool IsShortRoomCode(const char* roomCode) {
		if (!roomCode) {
			return false;
		}
		char buf[32];
		strncpy_s(buf, roomCode, _TRUNCATE);
		sf4e::TrimRoomCodeInPlace(buf);
		if (_strnicmp(buf, "SF4-", 4) != 0) {
			return false;
		}
		const char* token = buf + 4;
		size_t len = strlen(token);
		if (len < 4 || len > 8) {
			return false;
		}
		for (size_t i = 0; i < len; ++i) {
			if (!IsAlnumCodeChar(token[i])) {
				return false;
			}
		}
		return true;
	}

	bool FormatShortRoomCode(const char* token, char* outCode, int outCodeLen) {
		if (!token || !outCode || outCodeLen <= 0) {
			return false;
		}
		snprintf(outCode, outCodeLen, "SF4-%s", token);
		return true;
	}

	bool ParseShortRoomCode(const char* roomCode, char* outToken, int outTokenLen) {
		if (!IsShortRoomCode(roomCode) || !outToken || outTokenLen <= 0) {
			return false;
		}
		char buf[32];
		strncpy_s(buf, roomCode, _TRUNCATE);
		sf4e::TrimRoomCodeInPlace(buf);
		strncpy_s(outToken, outTokenLen, buf + 4, _TRUNCATE);
		return outToken[0] != 0;
	}

} // namespace launcher
} // namespace sf4e
