#pragma once
#include "PlayerPreferences.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// Additive settings-v1 extension. Missing room defaults adopt the existing
// battle preferences; decoding is transactional so malformed data stays intact.
// A public room's rules are kept apart, under "publicRules"; missing, they are
// the public default.
inline nlohmann::json RoomPreferences(const PlayerPreferences& value) {
    const auto& open = value.publicTableRules;
    return {{"name", value.roomName}, {"capacity", value.roomCapacity}, {"public", value.roomPublic},
        {"format", static_cast<int>(value.tableRules.format)},
        {"rotation", static_cast<int>(value.tableRules.rotation)},
        {"editionSelect", value.tableRules.editionSelect},
        {"roundCount", value.tableRules.roundCount}, {"roundTime", value.tableRules.roundTime},
        {"publicRules", {{"format", static_cast<int>(open.format)}, {"rotation", static_cast<int>(open.rotation)},
            {"editionSelect", open.editionSelect}, {"roundCount", open.roundCount}, {"roundTime", open.roundTime}}}};
}

// One set of table rules from a roomDefaults object, over `rules`; false when
// a value is the wrong type or out of range.
inline bool ReadTableRules(const nlohmann::json& object, room::Rules& rules) {
    for (const char* key : {"format", "rotation", "roundCount", "roundTime"})
        if (object.contains(key) && !object[key].is_number_integer()) return false;
    if (object.contains("editionSelect") && !object["editionSelect"].is_boolean()) return false;
    const auto format = object.value("format", static_cast<std::int64_t>(rules.format));
    const auto rotation = object.value("rotation", static_cast<std::int64_t>(rules.rotation));
    if (!room::ValidSetFormat(format) || rotation < 0 || rotation > 2) return false;
    const auto rounds = object.value("roundCount", static_cast<std::int64_t>(rules.roundCount));
    const auto time = object.value("roundTime", static_cast<std::int64_t>(rules.roundTime));
    if (rounds < 1 || rounds > 99 || time < 30 || time > 9999) return false;
    rules.format = static_cast<room::SetFormat>(format);
    rules.rotation = static_cast<room::RotationMode>(rotation);
    rules.roundCount = static_cast<std::uint8_t>(rounds);
    rules.roundTime = static_cast<std::uint16_t>(time);
    rules.editionSelect = object.value("editionSelect", rules.editionSelect);
    return true;
}

inline bool ReadRoomPreferences(const nlohmann::json& document, PlayerPreferences& value) {
    auto inherited = value;
    inherited.tableRules.format = room::SetFormat::Unlimited;
    inherited.tableRules.rotation = room::RotationMode::WinnerStays;
    inherited.tableRules.editionSelect = value.lobby.editionSelect;
    inherited.tableRules.roundCount = static_cast<std::uint8_t>(value.lobby.roundCount);
    inherited.tableRules.roundTime = static_cast<std::uint16_t>(value.lobby.roundTime);
    if (!document.contains("roomDefaults")) { value = inherited; return true; }
    const auto& defaults = document["roomDefaults"];
    if (!defaults.is_object()) return false;
    try {
        if (defaults.contains("capacity") && !defaults["capacity"].is_number_integer()) return false;
        if (defaults.contains("public") && !defaults["public"].is_boolean()) return false;
        PlayerPreferences candidate = inherited;
        candidate.roomName = defaults.value("name", candidate.roomName);
        const auto capacity = defaults.value("capacity", static_cast<std::int64_t>(candidate.roomCapacity));
        if (capacity < 2 || capacity > static_cast<std::int64_t>(room::MaxMembers)) return false;
        candidate.roomCapacity = static_cast<int>(capacity);
        candidate.roomPublic = defaults.value("public", candidate.roomPublic);
        if (!ReadTableRules(defaults, candidate.tableRules)) return false;
        if (defaults.contains("publicRules") &&
            (!defaults["publicRules"].is_object() || !ReadTableRules(defaults["publicRules"], candidate.publicTableRules))) return false;
        if (!candidate.Valid()) return false;
        value = std::move(candidate);
        return true;
    } catch (const nlohmann::json::exception&) { return false; }
}

} }
