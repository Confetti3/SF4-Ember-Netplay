#pragma once
#include "PlayerPreferences.hxx"
#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// How a saved profile's input delay loads. The number and Auto are stored
// apart, so neither stands for the other: "inputDelay" is the chosen number
// and "autoInputDelay" is whether Auto is on. The game always writes both
// (OverlayPrefs::QueuePlayerPreferences).
//
// - A profile without "autoInputDelay" keeps its saved manual number.
// - "autoInputDelay": true is Auto; false is the saved number, as chosen.
// - Anything else under "autoInputDelay" is not a choice the game wrote, so
//   the profile uses its manual number. New profiles start at zero.
inline void ReadInputDelayPreference(const nlohmann::json& saved, PlayerPreferences& value) {
    if (!saved.is_object()) return;
    const auto delay = saved.find("inputDelay");
    if (delay != saved.end() && delay->is_number_integer())
        value.inputDelay = SavedInputDelay(static_cast<int>((std::max)(static_cast<std::int64_t>(-1),
            (std::min)(delay->get<std::int64_t>(), static_cast<std::int64_t>(MaximumInputDelay + 1)))));
    const auto automatic = saved.find("autoInputDelay");
    value.autoInputDelay = automatic != saved.end() && automatic->is_boolean() && automatic->get<bool>();
}

} }
