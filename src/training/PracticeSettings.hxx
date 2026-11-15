#pragma once
#include "FrameMeter.hxx"
#include "MoveInputs.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

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
// A part of the HUD: its name in training.json, which is its row on the HUD
// page too, and that row's label.
struct HudPart { const char* key; const char* label; bool MeterOptions::* shown; };
constexpr HudPart HudParts[] = {{"hud_numbers", "training.meter.hud_numbers", &MeterOptions::numbers}, {"hud_bars", "training.meter.hud_bars", &MeterOptions::bars},
    {"hud_state", "training.meter.hud_state", &MeterOptions::state}, {"hud_shortcuts", "training.meter.hud_shortcuts", &MeterOptions::shortcuts},
    {"hud_status", "training.meter.hud_status", &MeterOptions::status}};
// The classic style's parts, on the same page while that style is chosen.
constexpr HudPart ClassicParts[] = {{"classic_data", "training.meter.classic_data", &MeterOptions::classicData},
    {"classic_p2", "training.meter.classic_p2", &MeterOptions::classicP2},
    {"classic_inputs", "training.meter.classic_inputs", &MeterOptions::classicInputs}, {"classic_colors", "training.meter.classic_colors", &MeterOptions::classicColors},
    {"classic_extras", "training.meter.classic_extras", &MeterOptions::classicExtras}, {"classic_slant", "training.meter.classic_slant", &MeterOptions::classicSlant}};
// The parts of Hitboxes, on its page.
constexpr HudPart HitboxParts[] = {{"hitboxes_hurt", "training.hitboxes.hurt", &MeterOptions::hurtBoxes},
    {"hitboxes_throw", "training.hitboxes.throw", &MeterOptions::throwBoxes}, {"hitboxes_skeleton", "training.hitboxes.skeleton", &MeterOptions::skeleton}};
// A choice's name in training.json and its label.
struct NamedChoice { const char* key; const char* label; };
// The input displays.
constexpr NamedChoice InputDisplayChoices[InputDisplays] = {{"off", "common.off"}, {"stick", "training.meter.inputs_stick"},
    {"arrows", "training.meter.inputs_arrows"}, {"both", "training.meter.inputs_both"}};
// The parts of the Data panel, on its page.
constexpr HudPart DataParts[] = {{"data_move", "training.data.move", &MeterOptions::dataMove},
    {"data_position", "training.data.position", &MeterOptions::dataPosition}, {"data_state", "training.data.state", &MeterOptions::dataState},
    {"data_gauges", "training.data.gauges", &MeterOptions::dataGauges}, {"data_juggle", "training.data.juggle", &MeterOptions::dataJuggle},
    {"data_charge", "training.data.charge", &MeterOptions::dataCharge}};
// Whose Data panel shows.
constexpr NamedChoice DataFighterChoices[DataFighters] = {{"both", "training.data.fighters.both"}, {"p1", "training.data.fighters.p1"},
    {"p2", "training.data.fighters.p2"}};
// The meter's drawing options: angled unless the style is "flat", the
// recovery number only for a literal true, 60 frames shown unless 90 or 120
// is saved, every part of the HUD unless it is saved as a literal false, and
// the standard size unless another one is saved.
inline MeterOptions ReadMeterOptions(const nlohmann::json& meter) {
    MeterOptions options;
    if (!meter.is_object()) return options;
    const auto style = meter.value("style", nlohmann::json());
    options.style = style == "flat" ? MeterStyle::Flat : style == "classic" ? MeterStyle::Classic : MeterStyle::Angled;
    options.recovery = meter.value("hud_recovery", nlohmann::json(false)) == true;
    const auto shown = meter.value("frames_shown", nlohmann::json());
    for (const int choice : MeterShownChoices) if (shown.is_number_integer() && shown == choice) options.shown = choice;
    const auto history = meter.value("history_frames", nlohmann::json());
    for (const int choice : MeterHistoryChoices) if (history.is_number_integer() && history == choice) options.history = choice;
    options.continuous = meter.value("after_pause", nlohmann::json()) == "continue";
    const auto inputs = meter.value("hud_inputs", nlohmann::json());
    for (int choice = 0; choice < InputDisplays; ++choice) if (inputs == InputDisplayChoices[choice].key) options.inputs = static_cast<InputDisplay>(choice);
    options.smooth = meter.value("scroll", nlohmann::json()) == "smooth";
    options.hitboxes = meter.value("hitboxes", nlohmann::json(false)) == true;
    options.data = meter.value("data", nlohmann::json(false)) == true;
    const auto fighters = meter.value("data_fighters", nlohmann::json()), opacity = meter.value("data_opacity", nlohmann::json());
    for (int choice = 0; choice < DataFighters; ++choice) if (fighters == DataFighterChoices[choice].key) options.dataFighters = static_cast<DataFighter>(choice);
    for (const int choice : DataOpacities) if (opacity.is_number_integer() && opacity == choice) options.dataOpacity = choice;
    const auto position = meter.value("position", nlohmann::json()), barHeight = meter.value("bar_height", nlohmann::json());
    for (const int choice : MeterPositions) if (position.is_number_integer() && position == choice) options.position = choice;
    for (const int choice : ClassicBarHeights) if (barHeight.is_number_integer() && barHeight == choice) options.barHeight = choice;
    for (const auto& part : HudParts) options.*part.shown = meter.value(part.key, nlohmann::json(true)) != false;
    for (const auto& part : ClassicParts) options.*part.shown = meter.value(part.key, nlohmann::json(true)) != false;
    for (const auto& part : HitboxParts) options.*part.shown = meter.value(part.key, nlohmann::json(true)) != false;
    for (const auto& part : DataParts) options.*part.shown = meter.value(part.key, nlohmann::json(true)) != false;
    options.oilFull = meter.value("oil_full", nlohmann::json(false)) == true;
    const auto size = meter.value("hud_size", nlohmann::json());
    for (int choice = 0; choice < HudSizes; ++choice) if (size.is_number_integer() && size == choice) options.size = choice;
    return options;
}
inline nlohmann::json MeterOptionsJson(const MeterOptions& options) {
    nlohmann::json meter{{"style", options.style == MeterStyle::Classic ? "classic" : options.style == MeterStyle::Flat ? "flat" : "angled"}, {"hud_recovery", options.recovery}, {"frames_shown", options.shown}, {"hud_size", options.size}, {"history_frames", options.history}, {"after_pause", options.continuous ? "continue" : "start_over"}, {"scroll", options.smooth ? "smooth" : "pages"},
        {"hud_inputs", InputDisplayChoices[static_cast<int>(options.inputs)].key},
        {"position", options.position}, {"bar_height", options.barHeight}, {"hitboxes", options.hitboxes},
        {"data", options.data}, {"data_fighters", DataFighterChoices[static_cast<int>(options.dataFighters)].key}, {"data_opacity", options.dataOpacity}};
    for (const auto& part : HudParts) meter[part.key] = options.*part.shown;
    for (const auto& part : ClassicParts) meter[part.key] = options.*part.shown;
    for (const auto& part : HitboxParts) meter[part.key] = options.*part.shown;
    for (const auto& part : DataParts) meter[part.key] = options.*part.shown;
    meter["oil_full"] = options.oilFull;
    return meter;
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
// The reply slots, like the recording slots: each holds one typed move, is on
// or off, and has a weight for the random draw (DummyPlan::order). The reply
// plays one of the slots that are on; with none on, a recording that is on.
struct ReplySlotSetting { std::string moves; bool on = false; int weight = 1; };
using ReplySlots = std::array<ReplySlotSetting, SlotCount>;
// A slot's typed moves as the plan plays them; empty text is no move. Invalid
// text leaves the output alone so no partial reply can be submitted.
inline bool BuildReply(const std::string& text, ReplyChoice& result, std::string& error) {
    std::vector<std::string> steps;
    if (!combo::ParseSteps(text, steps, error)) return false;
    if (!combo::Clean(text).empty() && steps.empty()) { error = "a reply needs a move"; return false; }
    // A reply plays from its first pressed frame, so a wait before it would be lost.
    combo::Step first;
    if (!steps.empty() && combo::ParseStep(steps[0], first, error) && combo::Wait(first)) {
        error = "a reply cannot start with a wait"; return false;
    }
    // Made for the dummy facing right, as a slot holds it; it is played for the way it faces.
    combo::Compiled compiled;
    if (!steps.empty() && !combo::Synthesize(steps, true, compiled, error)) return false;
    result = ReplyChoice{}; result.moves = std::move(compiled.frames); result.offset = compiled.offset; result.anchor = compiled.anchor; result.hold = compiled.hold;
    return true;
}
// The plan for the slots that are on, each made as BuildReply makes one;
// false with the first slot's error, leaving the output plan alone.
inline bool BuildSlotPlan(const ReplySlots& slots, const DummyPlan& settings, DummyPlan& result, std::string& error) {
    DummyPlan plan = settings; plan.choices.clear();
    for (const auto& slot : slots) {
        if (!slot.on || combo::Clean(slot.moves).empty()) continue;
        ReplyChoice choice;
        if (!BuildReply(slot.moves, choice, error)) return false;
        choice.weight = (std::max)(1, (std::min)(MaxReplyWeight, slot.weight));
        plan.choices.push_back(std::move(choice));
    }
    result = std::move(plan);
    return true;
}
} }
