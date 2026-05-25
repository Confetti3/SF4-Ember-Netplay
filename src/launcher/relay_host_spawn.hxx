#pragma once

#include <cstdint>

namespace sf4e {
namespace launcher {

	bool FetchAdvertiseRelayHost(char* outHost, int outHostLen);

	bool SpawnRelayHost(uint16_t sessionPort);

} // namespace launcher
} // namespace sf4e
