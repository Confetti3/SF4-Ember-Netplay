#pragma once
#include <cstddef>

namespace sf4e { namespace room {
// Room admission and GGPO match rosters are distinct even when every member
// watches the same table. The legacy protocol retains its four-user bound.
constexpr std::size_t MaxMembers = 16;
// Every room has eight tables, one for each pair of its sixteen members. A
// Classic room plays at the first four (room::TablesInPlay).
constexpr std::size_t TableCount = 8;
constexpr std::size_t ClassicTableCount = 4;
// An Arcade room's one-player cabinets, one for each member.
constexpr std::size_t CabinetCount = 2 * TableCount;
// A table's own name ("Beginners"), beside its number.
constexpr std::size_t MaximumTableNameBytes = 24;
constexpr std::size_t FighterCount = 2;
constexpr std::size_t MaxSpectators = MaxMembers - FighterCount;
constexpr std::size_t MaxMatchParticipants = FighterCount + MaxSpectators;
} }
