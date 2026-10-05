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
// - A profile from before Auto (1.0.2 and older) has no "autoInputDelay": it
//   is on Auto, and its number is kept, with a 0 from before 0 was withdrawn
//   loading as 1. The 0 never turns Auto off, and Auto never reads as 0.
// - "autoInputDelay": true is Auto; false is the saved number, as chosen.
// - Anything else under "autoInputDelay" is not a choice the game wrote, so
//   the profile is on Auto, as one from before Auto is.
inline void ReadInputDelayPreference(const nlohmann::json& saved, PlayerPreferences& value) {
    if (!saved.is_object()) return;
    const auto delay = saved.find("inputDelay");
    if (delay != saved.end() && delay->is_number_integer())
        value.inputDelay = SavedInputDelay(static_cast<int>((std::max)(static_cast<std::int64_t>(-1),
            (std::min)(delay->get<std::int64_t>(), static_cast<std::int64_t>(MaximumInputDelay + 1)))));
    const auto automatic = saved.find("autoInputDelay");
    value.autoInputDelay = automatic == saved.end() || !automatic->is_boolean() || automatic->get<bool>();
}

} }
