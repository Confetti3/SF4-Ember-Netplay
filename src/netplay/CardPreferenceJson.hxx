#pragma once
#include "PlayerPreferences.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {
// What Edit card keeps of how the player plays (PlayerPreferences::cardDevice and the three after it) and the award
// families the card shows (cardPins), each under its own key of the netplay section. A profile from before the card
// had them has none and loads as the defaults. A value that is missing, of another type or out of range loads as
// its default, one by one, and the rest load as saved: an unknown device is Automatic, unknown looking bits are
// dropped (of more than two the lowest two stay), and a pin that is not an award family's shape, a repeat, or a fifth
// is dropped.
inline void ReadCardPreferences(const nlohmann::json& saved, PlayerPreferences& value) {
    const PlayerPreferences defaults;
    value.cardDevice = defaults.cardDevice;
    value.cardShowLink = defaults.cardShowLink;
    value.cardShowRegion = defaults.cardShowRegion;
    value.cardLooking = defaults.cardLooking;
    value.cardPins = defaults.cardPins;
    if (!saved.is_object()) return;
    const auto device = saved.find("cardDevice");
    if (device != saved.end() && device->is_number_integer() && device->get<long long>() >= 0 && device->get<long long>() <= 5)
        value.cardDevice = device->get<int>();
    const auto flag = [&](const char* key, bool& member) {
        const auto it = saved.find(key);
        if (it != saved.end() && it->is_boolean()) member = it->get<bool>();
    };
    flag("cardShowLink", value.cardShowLink);
    flag("cardShowRegion", value.cardShowRegion);
    const auto looking = saved.find("cardLooking");
    if (looking != saved.end() && looking->is_number_integer() && looking->get<long long>() >= 0) value.cardLooking = profiles::LookingFromSaved(looking->get<std::uint64_t>());
    const auto pins = saved.find("cardPins");
    if (pins != saved.end() && pins->is_array()) {
        for (const auto& pin : *pins) {
            if (value.cardPins.size() == profiles::MaxPins) break;
            if (!pin.is_string()) continue;
            const auto id = pin.get<std::string>();
            if (profiles::FamilyIdShaped(id) && std::find(value.cardPins.begin(), value.cardPins.end(), id) == value.cardPins.end()) value.cardPins.push_back(id);
        }
    }
}
inline void WriteCardPreferences(const PlayerPreferences& value, nlohmann::json& saved) {
    saved["cardDevice"] = value.cardDevice;
    saved["cardShowLink"] = value.cardShowLink;
    saved["cardShowRegion"] = value.cardShowRegion;
    saved["cardLooking"] = value.cardLooking;
    saved["cardPins"] = value.cardPins;
}
} }
