#pragma once
#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <cstring>
#endif

namespace sf4e {

// A boolean environment switch: "1..." is on, "0..." is off, and anything else
// (unset, empty, too long) is `fallback`.
inline bool EnvFlag(const char* name, bool fallback = false) {
	char value[8] = {};
#ifdef _WIN32
	const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
	if (length == 0 || length >= sizeof(value)) return fallback;
#else
	const char* found = std::getenv(name);
	if (!found || !*found || std::strlen(found) >= sizeof(value)) return fallback;
	std::strcpy(value, found);
#endif
	return value[0] == '1' ? true : value[0] == '0' ? false : fallback;
}

}
