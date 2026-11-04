#pragma once
#include "GameCatalog.hxx"
#include "StageCatalog.hxx"
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>

namespace sf4e { namespace selection {
inline int ReadStage(const nlohmann::json& value) {
    // Check JSON before narrowing (including unsigned values above INT64_MAX);
    // which ids are stages is the game's to say.
    if (!value.is_number_integer() || value < 0 || value > (std::numeric_limits<std::int32_t>::max)() ||
        !ember::Catalog().validStage(value.get<std::int64_t>()))
        throw nlohmann::json::out_of_range::create(406, "Stage must be a supported versus stage ID", &value);
    return value.get<int>();
}
inline int PreferenceStage(const nlohmann::json& object, const char* key, int fallback) {
    const auto found = object.find(key);
    if (found != object.end()) {
        // Random is a saved local choice, never a wire value, so ReadStage stays strict.
        if (found->is_number_integer() && *found == RandomStageId) return RandomStageId;
        try { return ReadStage(*found); }
        catch (const nlohmann::json::exception&) {}
    }
    return NormalizeStageChoice(fallback);
}
} }
