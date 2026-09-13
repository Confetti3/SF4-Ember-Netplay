#pragma once
#include "PlayerPreferences.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

// Additive settings-v1 extension. Missing room defaults adopt the existing
// battle preferences; decoding is transactional so malformed data stays intact.
inline nlohmann::json RoomPreferences(const PlayerPreferences& value) {
    return {{"name", value.roomName}, {"capacity", value.roomCapacity},
        // Keep the legacy keys in the persisted shape for migration readers,
        // but always write the only supported custom-room rule set.
        {"format", static_cast<int>(room::SetFormat::Unlimited)},
        {"rotation", static_cast<int>(room::RotationMode::WinnerStays)},
        {"editionSelect", value.tableRules.editionSelect},
        {"roundCount", value.tableRules.roundCount}, {"roundTime", value.tableRules.roundTime}};
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
        for (const char* key : {"capacity", "format", "rotation", "roundCount", "roundTime"})
            if (defaults.contains(key) && !defaults[key].is_number_integer()) return false;
        PlayerPreferences candidate = inherited;
        candidate.roomName = defaults.value("name", candidate.roomName);
        const auto capacity = defaults.value("capacity", static_cast<std::int64_t>(candidate.roomCapacity));
        if (capacity < 2 || capacity > static_cast<std::int64_t>(room::MaxMembers)) return false;
        candidate.roomCapacity = static_cast<int>(capacity);
        const auto format = defaults.value("format", static_cast<std::int64_t>(candidate.tableRules.format));
        const auto rotation = defaults.value("rotation", static_cast<std::int64_t>(candidate.tableRules.rotation));
        if ((format != 0 && format != 1 && format != 2 && format != 3 && format != 5) || rotation < 0 || rotation > 2) return false;
        // Legacy values are parsed for compatibility, then normalized so a
        // migrated profile cannot reintroduce set rotation semantics.
        candidate.tableRules.format = room::SetFormat::Unlimited;
        candidate.tableRules.rotation = room::RotationMode::WinnerStays;
        const auto rounds = defaults.value("roundCount", static_cast<std::int64_t>(candidate.tableRules.roundCount));
        const auto time = defaults.value("roundTime", static_cast<std::int64_t>(candidate.tableRules.roundTime));
        if (rounds < 1 || rounds > 99 || time < 30 || time > 9999) return false;
        candidate.tableRules.roundCount = static_cast<std::uint8_t>(rounds);
        candidate.tableRules.roundTime = static_cast<std::uint16_t>(time);
        candidate.tableRules.editionSelect = defaults.value("editionSelect", candidate.tableRules.editionSelect);
        if (!candidate.Valid()) return false;
        value = std::move(candidate);
        return true;
    } catch (const nlohmann::json::exception&) { return false; }
}

} }
