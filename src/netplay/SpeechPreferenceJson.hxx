#pragma once
#include "PlayerPreferences.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {
// Speech is one object in the netplay section. A profile from before speech
// has none; a field that is missing, of another type or out of range loads as
// its default, and the other fields load as saved.
inline SpeechPreferences ReadSpeechPreferences(const nlohmann::json& saved) {
    SpeechPreferences value;
    if (!saved.is_object()) return value;
    const auto found = saved.find("speech");
    if (found == saved.end() || !found->is_object()) return value;
    const auto flag = [&](const char* key, bool& member) {
        const auto it = found->find(key);
        if (it != found->end() && it->is_boolean()) member = it->get<bool>();
    };
    const auto number = [&](const char* key, int& member, int low, int high, int step) {
        const auto it = found->find(key);
        if (it != found->end() && it->is_number_integer() && *it >= low && *it <= high && it->get<int>() % step == 0) member = it->get<int>();
    };
    flag("enabled", value.enabled); flag("details", value.details); flag("hints", value.hints); flag("position", value.position);
    number("rate", value.rate, -10, 10, 1); number("volume", value.volume, 10, 100, 10);
    return value;
}
inline void WriteSpeechPreferences(const PlayerPreferences& value, nlohmann::json& saved) {
    const auto& speech = value.speech;
    saved["speech"] = {{"enabled", speech.enabled}, {"rate", speech.rate}, {"volume", speech.volume}, {"details", speech.details}, {"hints", speech.hints}, {"position", speech.position}};
}
} }
