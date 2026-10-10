#pragma once
#include "FrameMeter.hxx"
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
// The meter's drawing options: angled unless the style is "flat", the
// recovery number only for a literal true, and 60 frames shown unless 90 or
// 120 is saved.
inline MeterOptions ReadMeterOptions(const nlohmann::json& meter) {
    MeterOptions options;
    if (!meter.is_object()) return options;
    options.flat = meter.value("style", nlohmann::json()) == "flat";
    options.recovery = meter.value("hud_recovery", nlohmann::json(false)) == true;
    const auto shown = meter.value("frames_shown", nlohmann::json());
    for (const int choice : MeterShownChoices) if (shown.is_number_integer() && shown == choice) options.shown = choice;
    return options;
}
inline nlohmann::json MeterOptionsJson(const MeterOptions& options) {
    return {{"style", options.flat ? "flat" : "angled"}, {"hud_recovery", options.recovery}, {"frames_shown", options.shown}};
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
// The dummy reply's pick list: generic notation that reads the same for every
// fighter (a reversal, back and forward dashes, a throw, jumps, a low jab and
// two quarter circles). Character-specific names are not read, so none are offered.
constexpr const char* ReplyPresets[] = {"623P", "44", "66", "LP+LK", "8", "9", "2LK", "214K", "236P"};
constexpr int ReplyPresetCount = static_cast<int>(sizeof(ReplyPresets) / sizeof(*ReplyPresets));
// The preset a reply's text spells, compared by its parsed moves, so a saved
// "lp+lk" or "throw" (migrated to 5LP+LK) is the LP+LK preset; -1 for none,
// including empty or unreadable text.
inline int ReplyPresetIndex(const std::string& text) {
    std::vector<std::string> steps, preset; std::string error;
    if (!combo::ParseSteps(text, steps, error) || steps.empty()) return -1;
    for (int i = 0; i < ReplyPresetCount; ++i)
        if (combo::ParseSteps(ReplyPresets[i], preset, error) && preset == steps) return i;
    return -1;
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
