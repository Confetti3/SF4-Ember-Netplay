#pragma once
#include "BoolPreferences.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// Each on/off preference loads as saved, or as its default when the profile
// has none. A value that is not true or false throws, as it always has, and
// the game reports the preferences as unreadable.
template<class Preferences>
inline void ReadBoolPreference(const nlohmann::json& saved, Preferences& value, const BoolPreferenceMember<Preferences>& preference) {
    static const Preferences defaults;
    value.*preference.member = saved.value(preference.key, defaults.*preference.member);
}

template<class Preferences, std::size_t N>
inline void ReadBoolPreferences(const nlohmann::json& saved, Preferences& value, const BoolPreferenceMember<Preferences> (&preferences)[N]) {
    for (const auto& preference : preferences) ReadBoolPreference(saved, value, preference);
}

template<class Preferences, std::size_t N>
inline void WriteBoolPreferences(const Preferences& value, nlohmann::json& saved, const BoolPreferenceMember<Preferences> (&preferences)[N]) {
    for (const auto& preference : preferences) saved[preference.key] = value.*preference.member;
}

inline void ReadBoolPreferences(const nlohmann::json& saved, PlayerPreferences& value) {
    ReadBoolPreferences(saved, value, BoolPreferences);
}

inline void WriteBoolPreferences(const PlayerPreferences& value, nlohmann::json& saved) {
    WriteBoolPreferences(value, saved, BoolPreferences);
}

} }
