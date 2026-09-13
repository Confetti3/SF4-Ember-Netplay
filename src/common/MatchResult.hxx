#pragma once
#include <cstdint>

namespace sf4e { namespace room {
enum class MatchResult : std::uint8_t {
    P1Win = 0,
    P2Win,
    Draw,
    Cancel,
    Abort,
};
} }
