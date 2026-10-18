#pragma once
#include "PlayerPreferences.hxx"
#include <nlohmann/json.hpp>
#include <limits>

namespace sf4e { namespace netplay {
inline int ReadMusicChoice(const nlohmann::json& saved, const char* key, bool allowRandom) {
    const auto value = saved.find(key);
    if (value == saved.end() || !value->is_number_integer()) return music::Default;
    if (value->is_number_unsigned()) {
        const auto number = value->get<std::uint64_t>();
        return number <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
            ? music::Normalize(static_cast<std::int64_t>(number), allowRandom) : music::Default;
    }
    return music::Normalize(value->get<std::int64_t>(), allowRandom);
}
inline void ReadMusicPreferences(const nlohmann::json& saved, PlayerPreferences& preferences) {
    preferences.lobbyMusic = ReadMusicChoice(saved, "lobbyMusic", false);
    preferences.matchMusic = ReadMusicChoice(saved, "matchMusic", true);
}
} }
