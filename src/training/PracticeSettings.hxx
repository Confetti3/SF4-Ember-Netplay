#pragma once
#include "MoveInputs.hxx"
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <utility>

// Pure decoding of the lab's saved settings; no game or overlay state.
namespace sf4e { namespace training {
// Offsets from F1, -1 unbound. Only keys left free by Ember and Steam.
inline bool FreePositionKey(const nlohmann::json& key) {
    // Compare as values: json compares a huge unsigned number as if it were signed.
    if (!key.is_number_integer() || (key.is_number_unsigned() && key.get<std::uint64_t>() >= 12)) return false;
    const auto value = key.get<std::int64_t>();
    return value >= -1 && value < 12 && value != 9 && (value < 4 || value > 7);
}
inline std::array<int, 2> ReadPositionKeys(const nlohmann::json& keys) {
    std::array<int, 2> result{{1, 10}};
    const bool listed = keys.is_array() && keys.size() == 6;
    const auto reset = listed ? keys[1] : keys.is_object() ? keys.value("reset_position", nlohmann::json()) : nlohmann::json();
    const auto save = listed ? keys[5] : keys.is_object() ? keys.value("save_position", nlohmann::json()) : nlohmann::json();
    if (FreePositionKey(reset)) result[0] = reset.get<int>();
    if (FreePositionKey(save)) result[1] = save.get<int>();
    // Reset wins a collision. Use the other default for Save when Reset
    // already owns F11, including when Save was missing or invalid.
    if (result[0] >= 0 && result[0] == result[1]) result[1] = result[0] == 10 ? 1 : 10;
    return result;
}
// Old replies accepted these shared names without a fighter database.
// Resolve them only when loading a saved reply; new input uses notation.
inline std::string MigrateReplyMoves(const std::string& text) {
    struct Alias { const char* name; const char* notation; };
    static const Alias aliases[] = {
        {"focus", "MP+MK"}, {"focusattack", "MP+MK"},
        {"redfocus", "LP+MP+MK"}, {"redfocusattack", "LP+MP+MK"},
        {"falv1", "MP+MK"}, {"falv2", "[MP+MK]"}, {"falv3", "[MP+MK]"},
        {"focusattacklv1", "MP+MK"}, {"focusattacklv2", "[MP+MK]"}, {"focusattacklv3", "[MP+MK]"},
        {"redfalv1", "LP+MP+MK"}, {"redfalv2", "[LP+MP+MK]"}, {"redfalv3", "[LP+MP+MK]"},
        {"redfocusattacklv1", "LP+MP+MK"}, {"redfocusattacklv2", "[LP+MP+MK]"}, {"redfocusattacklv3", "[LP+MP+MK]"},
        {"throw", "LP+LK"}, {"backthrow", "4LP+LK"}, {"taunt", "HP+HK"},
        {"dash", "66"}, {"backdash", "44"}, {"jump", "8"}, {"jumpforward", "9"}, {"jumpback", "7"}
    };
    std::string line;
    bool migrated = false;
    for (auto token : combo::Tokens(text)) {
        std::string cancel;
        if (token.size() > 3 && combo::Fold(token.substr(0, 3), std::tolower) == "xx ") {
            cancel = "xx "; token = token.substr(3);
        }
        std::string key;
        for (unsigned char c : token) if (std::isalnum(c)) key += static_cast<char>(std::tolower(c));
        for (const auto& alias : aliases) if (key == alias.name) { token = alias.notation; migrated = true; break; }
        line += (line.empty() ? "" : " > ") + cancel + token;
    }
    std::vector<std::string> steps; std::string error;
    if (!migrated || !combo::ParseSteps(line, steps, error)) return combo::Clean(text);
    line.clear();
    for (const auto& step : steps) line += (line.empty() ? "" : " > ") + step;
    return line;
}
// Empty text selects a recording. Invalid nonempty text is never that path,
// and leaves the output plan alone so no partial reply can be submitted.
inline bool BuildReplyPlan(const std::string& text, const DummyPlan& settings, DummyPlan& result, std::string& error) {
    std::vector<std::string> steps;
    if (!combo::ParseSteps(text, steps, error)) return false;
    if (!combo::Clean(text).empty() && steps.empty()) { error = "a reply needs a move"; return false; }
    DummyPlan plan = settings;
    for (int side = 0; side < 2; ++side) plan.moves[side] = steps.empty() ? std::vector<Input>{} : combo::Synthesize(steps, side == 0);
    result = std::move(plan);
    return true;
}
} }
