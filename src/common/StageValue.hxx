#pragma once
#include "StageCatalog.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace selection {
inline int ReadStage(const nlohmann::json& value) {
    // Check JSON before narrowing (including unsigned values above INT64_MAX).
    if (!value.is_number_integer() || value < 0 || value > 29 || !FindStage(value.get<int>()))
        throw nlohmann::json::out_of_range::create(406, "Stage must be a supported versus stage ID", &value);
    return value.get<int>();
}
inline int PreferenceStage(const nlohmann::json& object, const char* key, int fallback) {
    const auto found = object.find(key);
    if (found != object.end()) {
        try { return ReadStage(*found); }
        catch (const nlohmann::json::exception&) {}
    }
    return NormalizeStage(fallback);
}
} }
