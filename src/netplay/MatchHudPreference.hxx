#pragma once
#include "PlayerPreferences.hxx"
#include <cstdint>
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// The split HUD's name offset from a saved profile: a whole number from
// -MaxMatchHudNameOffset to MaxMatchHudNameOffset. Anything else, including a
// number too large for an int, is Default (0); the rest of the profile loads
// as saved. A profile from before 1.1.0 has none and is at Default.
inline int ReadMatchHudNameOffset(const nlohmann::json& saved) {
    if (!saved.is_object()) return 0;
    const auto found = saved.find("matchHudNameOffset");
    if (found == saved.end() || !found->is_number_integer()) return 0;
    if (found->is_number_unsigned()) {
        const auto value = found->get<std::uint64_t>();
        return value <= static_cast<std::uint64_t>(MaxMatchHudNameOffset) ? static_cast<int>(value) : 0;
    }
    const auto value = found->get<std::int64_t>();
    return value >= -MaxMatchHudNameOffset && value <= MaxMatchHudNameOffset ? static_cast<int>(value) : 0;
}

} }
