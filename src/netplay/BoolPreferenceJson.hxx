#pragma once
#include "BoolPreferences.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// Each on/off preference loads as saved, or as its default when the profile
// has none. A value that is not true or false throws, as it always has, and
// the game reports the preferences as unreadable.
inline void ReadBoolPreferences(const nlohmann::json& saved, PlayerPreferences& value) {
    static const PlayerPreferences defaults;
    for (const auto& preference : BoolPreferences) value.*preference.member = saved.value(preference.key, defaults.*preference.member);
}

inline void WriteBoolPreferences(const PlayerPreferences& value, nlohmann::json& saved) {
    for (const auto& preference : BoolPreferences) saved[preference.key] = value.*preference.member;
}

} }
