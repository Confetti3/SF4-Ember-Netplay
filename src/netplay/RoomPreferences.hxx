#pragma once
#include "PlayerPreferences.hxx"
#include <cstdint>
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// Additive settings-v1 extension. Missing room defaults adopt the existing
// battle preferences; decoding is transactional so malformed data stays intact.
// A public room's rules are kept apart, under "publicRules"; missing, they are
// the public default.
// One set of table rules, whole: ReadTableRules reads it back.
inline nlohmann::json TableRulesJson(const room::Rules& rules) {
    return {{"format", static_cast<int>(rules.format)}, {"rotation", static_cast<int>(rules.rotation)},
        {"editionSelect", rules.editionSelect}, {"roundCount", rules.roundCount}, {"roundTime", rules.roundTime}};
}
// The private room's rules sit beside the room's own fields, the public room's under "publicRules".
inline nlohmann::json RoomPreferences(const PlayerPreferences& value) {
    auto json = TableRulesJson(value.tableRules);
    json["name"] = value.roomName; json["capacity"] = value.roomCapacity; json["public"] = value.roomPublic;
    json["publicRules"] = TableRulesJson(value.publicTableRules);
    return json;
}

// One set of table rules from a roomDefaults object, over `rules`; false when
// a value is the wrong type or the rules are not valid (room::Rules::Valid).
// "training" and "timeBeforePractice", saved while Practice match was a table
// rule, are left unread, so defaults saved with them still load.
inline bool ReadTableRules(const nlohmann::json& object, room::Rules& rules) {
    for (const char* key : {"format", "rotation", "roundCount", "roundTime"})
        if (object.contains(key) && !object[key].is_number_integer()) return false;
    if (object.contains("editionSelect") && !object["editionSelect"].is_boolean()) return false;
    const auto number = [&](const char* key, std::int64_t current, std::int64_t largest, std::int64_t& read) {
        read = object.value(key, current);
        return read >= 0 && read <= largest;
    };
    // The bounds only keep the casts exact; Valid says what a table offers.
    // A missing key keeps the value read over.
    std::int64_t format = 0, rotation = 0, rounds = 0, time = 0;
    if (!number("format", static_cast<std::int64_t>(rules.format), UINT8_MAX, format) ||
        !number("rotation", static_cast<std::int64_t>(rules.rotation), UINT8_MAX, rotation) ||
        !number("roundCount", rules.roundCount, UINT8_MAX, rounds) || !number("roundTime", rules.roundTime, UINT16_MAX, time)) return false;
    rules.format = static_cast<room::SetFormat>(format);
    rules.rotation = static_cast<room::RotationMode>(rotation);
    rules.roundCount = static_cast<std::uint8_t>(rounds);
    rules.roundTime = static_cast<std::uint16_t>(time);
    rules.editionSelect = object.value("editionSelect", rules.editionSelect);
    return rules.Valid();
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
