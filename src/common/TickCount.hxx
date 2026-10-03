#pragma once
// GetTickCount64 on every platform: milliseconds from a monotonic clock. The
// Iroh room and the helper client call it by its Windows name, so other
// platforms get a function of that name over std::chrono::steady_clock.
#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <cstdint>

inline std::uint64_t GetTickCount64() {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}
#endif
