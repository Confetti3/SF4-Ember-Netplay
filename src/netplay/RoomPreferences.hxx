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
    nlohmann::json json{{"format", static_cast<int>(rules.format)}, {"rotation", static_cast<int>(rules.rotation)},
        {"editionSelect", rules.editionSelect}, {"roundCount", rules.roundCount}, {"roundTime", rules.roundTime},
        {"instantRematch", rules.instantRematch}};
    if (rules.winnerKeepsFighter) json["winnerKeepsFighter"] = true;
    return json;
}
// A saved delay policy: Shared only when the player chose it; Own when chosen
// or when the key is missing (Own is the default since 1.5.3).
inline room::DelayPolicy DelayPolicyOr(const nlohmann::json& saved) {
    return saved.value("delay", std::string()) == "shared" ? room::DelayPolicy::Shared : room::DelayPolicy::Own;
}

// The private room's rules sit beside the room's own fields, the public room's under "publicRules".
inline nlohmann::json RoomPreferences(const PlayerPreferences& value) {
    auto json = TableRulesJson(value.tableRules);
    json["name"] = value.roomName; json["capacity"] = value.roomCapacity; json["public"] = value.roomPublic;
    json["publicRules"] = TableRulesJson(value.publicTableRules);
    // Both are written. A document without the key, as every save before 1.5.3
    // that kept the old Shared default, reads as Own (DelayPolicyOr).
    json["delay"] = value.roomDelayPolicy == room::DelayPolicy::Own ? "own" : "shared";
    // Only a count other than the default four is written.
    if (value.roomTableCount != static_cast<int>(room::ClassicTableCount)) json["tableCount"] = value.roomTableCount;
    // Only on is written; a document without the key reads as off.
    if (value.roomWiredOnly) json["wiredOnly"] = true;
    if (value.publicRoomType != room::ClassicRoom) json["publicType"] = static_cast<int>(value.publicRoomType);
    if (value.publicArcadeTables != static_cast<int>(room::TableCount)) json["publicTables"] = value.publicArcadeTables;
    if (!value.publicRoomTemplate.empty()) json["publicTemplate"] = value.publicRoomTemplate;
    if (!value.roomTemplates.empty()) {
        auto& templates = json["templates"] = nlohmann::json::array();
        for (const auto& saved : value.roomTemplates) {
            auto tables = nlohmann::json::array();
            for (std::size_t i = 0; i < room::TableCount; ++i) {
                auto table = TableRulesJson(saved.tables.rules[i]);
                if (!saved.tables.names[i].empty()) table["name"] = saved.tables.names[i];
                tables.push_back(std::move(table));
            }
            templates.push_back({{"name", saved.name}, {"room", saved.roomName}, {"capacity", saved.capacity}, {"public", saved.isPublic},
                {"practice", saved.practice}, {"delay", saved.delayPolicy == room::DelayPolicy::Own ? "own" : "shared"}, {"tables", std::move(tables)}});
            if (saved.tableCount != static_cast<int>(room::ClassicTableCount)) templates.back()["tableCount"] = saved.tableCount;
            if (saved.wiredOnly) templates.back()["wiredOnly"] = true;
        }
    }
    return json;
}

// One set of table rules from a roomDefaults object, over `rules`; false when
// a value is the wrong type or the rules are not valid (room::Rules::Valid).
// "training" and "timeBeforePractice", saved while Practice match was a table
// rule, are left unread, so defaults saved with them still load.
inline bool ReadTableRules(const nlohmann::json& object, room::Rules& rules) {
    for (const char* key : {"format", "rotation", "roundCount", "roundTime"})
        if (object.contains(key) && !object[key].is_number_integer()) return false;
    for (const char* key : {"editionSelect", "instantRematch", "winnerKeepsFighter"})
        if (object.contains(key) && !object[key].is_boolean()) return false;
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
    rules.instantRematch = object.value("instantRematch", rules.instantRematch);
    rules.winnerKeepsFighter = object.value("winnerKeepsFighter", rules.winnerKeepsFighter);
    return rules.Valid();
}

// The saved templates; one that cannot be read whole is left out, so the
// others and the room defaults still load.
inline std::vector<RoomTemplate> ReadRoomTemplates(const nlohmann::json& defaults) {
    std::vector<RoomTemplate> templates;
    const auto list = defaults.find("templates");
    if (list == defaults.end() || !list->is_array()) return templates;
    for (const auto& item : *list) {
        if (templates.size() == MaxRoomTemplates) break;
        try {
            RoomTemplate saved;
            saved.name = item.at("name").get<std::string>(); saved.roomName = item.at("room").get<std::string>();
            saved.capacity = item.at("capacity").get<int>(); saved.isPublic = item.at("public").get<bool>();
            saved.practice = item.value("practice", false);
            saved.delayPolicy = DelayPolicyOr(item);
            saved.tableCount = item.value("tableCount", static_cast<int>(room::ClassicTableCount));
            saved.wiredOnly = item.value("wiredOnly", false);
            const auto& tables = item.at("tables");
            if (!tables.is_array() || tables.size() != room::TableCount) continue;
            bool read = true;
            for (std::size_t i = 0; i < room::TableCount && read; ++i) {
                read = tables[i].is_object() && ReadTableRules(tables[i], saved.tables.rules[i]);
                if (read && tables[i].contains("name")) saved.tables.names[i] = tables[i].at("name").get<std::string>();
            }
            if (read && saved.Valid()) templates.push_back(std::move(saved));
        } catch (const nlohmann::json::exception&) {}
    }
    return templates;
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
        if (defaults.contains("wiredOnly") && !defaults["wiredOnly"].is_boolean()) return false;
        if (defaults.contains("delay") && (!defaults["delay"].is_string() ||
            (defaults["delay"].get<std::string>() != "own" && defaults["delay"].get<std::string>() != "shared"))) return false;
        PlayerPreferences candidate = inherited;
        candidate.roomName = defaults.value("name", candidate.roomName);
        const auto capacity = defaults.value("capacity", static_cast<std::int64_t>(candidate.roomCapacity));
        if (capacity < 2 || capacity > static_cast<std::int64_t>(room::MaxMembers)) return false;
        candidate.roomCapacity = static_cast<int>(capacity);
        if (defaults.contains("tableCount") && !defaults["tableCount"].is_number_integer()) return false;
        const auto tableCount = defaults.value("tableCount", static_cast<std::int64_t>(room::ClassicTableCount));
        if (tableCount < 1 || tableCount > static_cast<std::int64_t>(room::TableCount)) return false;
        candidate.roomTableCount = static_cast<int>(tableCount);
        candidate.roomPublic = defaults.value("public", candidate.roomPublic);
        candidate.roomDelayPolicy = DelayPolicyOr(defaults);
        candidate.roomWiredOnly = defaults.value("wiredOnly", false);
        const auto publicType = defaults.value("publicType", static_cast<std::int64_t>(room::ClassicRoom));
        if (publicType < room::ClassicRoom || publicType > room::KingRoom) return false;
        candidate.publicRoomType = static_cast<room::RoomType>(publicType);
        if (defaults.contains("publicTables") && !defaults["publicTables"].is_number_integer()) return false;
        const auto publicTables = defaults.value("publicTables", static_cast<std::int64_t>(room::TableCount));
        if (publicTables < 1 || publicTables > static_cast<std::int64_t>(room::TableCount)) return false;
        candidate.publicArcadeTables = static_cast<int>(publicTables);
        candidate.publicRoomTemplate = defaults.value("publicTemplate", std::string());
        if (!ReadTableRules(defaults, candidate.tableRules)) return false;
        if (defaults.contains("publicRules") &&
            (!defaults["publicRules"].is_object() || !ReadTableRules(defaults["publicRules"], candidate.publicTableRules))) return false;
        candidate.roomTemplates = ReadRoomTemplates(defaults);
        if (!candidate.Valid()) return false;
        value = std::move(candidate);
        return true;
    } catch (const nlohmann::json::exception&) { return false; }
}

} }
