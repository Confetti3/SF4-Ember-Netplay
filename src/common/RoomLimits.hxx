#pragma once
#include <cstddef>

namespace sf4e { namespace room {
// Room admission and GGPO match rosters are distinct even when every member
// watches the same table. The legacy protocol retains its four-user bound.
constexpr std::size_t MaxMembers = 16;
constexpr std::size_t TableCount = 4;
constexpr std::size_t FighterCount = 2;
constexpr std::size_t MaxSpectators = MaxMembers - FighterCount;
constexpr std::size_t MaxMatchParticipants = FighterCount + MaxSpectators;
} }
