#pragma once

#include <cstdint>

namespace sf4e {
namespace launcher {

	bool TryMapUpnpPort(uint16_t externalPort, uint16_t internalPort, const char* protocol, const char* description);

} // namespace launcher
} // namespace sf4e
