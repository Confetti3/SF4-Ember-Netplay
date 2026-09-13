#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>

namespace sf4e { namespace selection {

// JSON's integer conversion narrows silently. Check the original value before
// converting native character-selection fields to the game's byte representation.
inline std::uint8_t ReadByte(const nlohmann::json& value) {
    if (!value.is_number_integer() || value < 0 || value > 255)
        throw nlohmann::json::out_of_range::create(406, "Selection value must be an integer from 0 to 255", &value);
    return value.get<std::uint8_t>();
}

// Local preferences are recoverable. Incoming protocol values use ReadByte and
// reject malformed input instead of repairing an opponent's selection silently.
inline std::uint8_t PreferenceByte(const nlohmann::json& object, const char* key, std::uint8_t fallback) {
    const auto found = object.find(key);
    if (found == object.end()) return fallback;
    try { return ReadByte(*found); }
    catch (const nlohmann::json::exception&) { return fallback; }
}

} }
