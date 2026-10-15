#pragma once
#include "PlayerPreferences.hxx"
#include <cstdint>
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// How a saved profile's input delay loads. The launcher (netplay_persist.cxx)
// reads only the number, through ReadSavedInputDelay; the game reads both.
// The number and Auto are stored apart, so neither stands for the other:
// "inputDelay" is the chosen number and "autoInputDelay" is whether Auto is
// on. The game always writes both (OverlayPrefs::QueuePlayerPreferences).
//
// - "inputDelay" is a whole number from MinimumInputDelay to
//   MaximumInputDelay, 0 included. Anything else (missing, another type, a
//   fraction, out of range) is DefaultInputDelay, never 0: the type and range
//   are checked before the value is converted to int.
// - A profile from before Auto (1.0.2 and older) has no "autoInputDelay": it
//   is on Auto, whatever number it kept, 0 included. Auto never readies with
//   that number (AutoInputDelay), and leaving Auto starts at
//   MinimumInputDelay whatever it was (StepInputDelay).
// - "autoInputDelay": true is Auto; false is the saved number, as chosen.
// - Anything else under "autoInputDelay" is not a choice the game wrote, so
//   the profile is on Auto, as one from before Auto is.
inline int ReadSavedInputDelay(const nlohmann::json& saved) {
    // find is end() for anything but an object. An unsigned number past the
    // signed range reads as negative, so it is out of range too.
    const auto delay = saved.find("inputDelay");
    const std::int64_t number = delay != saved.end() && delay->is_number_integer() ? delay->get<std::int64_t>() : -1;
    return number >= MinimumInputDelay && number <= MaximumInputDelay ? static_cast<int>(number) : DefaultInputDelay;
}

inline void ReadInputDelayPreference(const nlohmann::json& saved, PlayerPreferences& value) {
    if (!saved.is_object()) return;
    value.inputDelay = ReadSavedInputDelay(saved);
    const auto automatic = saved.find("autoInputDelay");
    value.autoInputDelay = automatic == saved.end() || !automatic->is_boolean() || automatic->get<bool>();
}

} }
