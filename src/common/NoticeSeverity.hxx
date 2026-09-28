#pragma once

#include <cstdint>

namespace sf4e {

// Severity of a netplay notice. Info and Warning expire on their own. An
// Error stays until a newer notice replaces it or a new session starts.
enum class NoticeSeverity : std::uint8_t { Info = 0, Warning = 1, Error = 2 };

}
