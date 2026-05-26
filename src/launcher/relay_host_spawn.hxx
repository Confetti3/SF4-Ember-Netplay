#pragma once

#include <cstdint>

namespace sf4e {
namespace launcher {

	bool FetchAdvertiseRelayHost(char* outHost, int outHostLen);

	bool SpawnRelayHost(uint16_t sessionPort, unsigned long* outPid = nullptr);

	unsigned long GetRelayHostPid();

	void StopRelayHost();

} // namespace launcher
} // namespace sf4e
