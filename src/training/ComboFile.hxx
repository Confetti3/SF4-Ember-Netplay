#pragma once
#include "RecordingTypes.hxx"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// A saved combo as a file the phone can download and another player can import,
// and later play as a trial: {"format":"ember-combo","version":1,"id","name",
// "fighter","hits","created","start":{"x":[p1,p2]},"notation","frames":[[mapped,raw,count],...]}.
// The frames are Player 1's as a slot holds them (for a fighter facing right),
// each run of equal frames written once. A file comes from anyone, so reading
// one checks every field and keeps nothing it does not name.
namespace sf4e { namespace training {
constexpr const char* ComboFormat = "ember-combo";
constexpr int ComboVersion = 1;
// A full 120 s combo with a different press every frame is about 100 KB.
constexpr std::size_t ComboFileBytes = 128 * 1024;
constexpr std::size_t ComboNameBytes = 80, ComboNotationBytes = 512;
struct SavedCombo {
    std::string id, name, notation;
    int fighter = -1, hits = 0;   // fighter: the game's id for Player 1's fighter, -1 unknown
    std::int64_t created = 0;     // seconds since 1970
    float x[2] = {0, 0};
    std::vector<Input> frames;
};
// What the list shows of a saved combo.
struct ComboSummary { std::string id, name; int fighter = -1, hits = 0, length = 0; std::int64_t created = 0; };
// An id is a file name: lowercase letters, digits and dashes only.
inline bool ValidComboId(const std::string& id) {
    return !id.empty() && id.size() <= 32 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
// Text with no control characters, within a byte limit.
inline bool CleanText(const std::string& text, std::size_t limit) {
    return text.size() <= limit && std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
inline std::string EncodeCombo(const SavedCombo& combo) {
    nlohmann::json runs = nlohmann::json::array();
    for (const auto& frame : combo.frames) {
        const unsigned mapped = frame.mapped & FightButtons, raw = frame.raw & FightButtons;
        if (!runs.empty() && runs.back()[0] == mapped && runs.back()[1] == raw) runs.back()[2] = runs.back()[2].get<int>() + 1;
        else runs.push_back({mapped, raw, 1});
    }
    return nlohmann::json{{"format", ComboFormat}, {"version", ComboVersion}, {"id", combo.id}, {"name", combo.name}, {"fighter", combo.fighter},
        {"hits", combo.hits}, {"created", combo.created}, {"start", {{"x", {combo.x[0], combo.x[1]}}}}, {"notation", combo.notation}, {"frames", runs}}.dump();
}
// False with a reason a player can read. Bits outside the fight buttons are cleared, as a recording does.
inline bool DecodeCombo(const std::string& text, SavedCombo& combo, std::string& error) {
    combo = SavedCombo{};
    if (text.size() > ComboFileBytes) { error = "the file is too big"; return false; }
    const auto value = nlohmann::json::parse(text, nullptr, false);
    if (!value.is_object() || value.value("format", nlohmann::json()) != ComboFormat) { error = "this is not a combo file"; return false; }
    const auto version = value.value("version", nlohmann::json());
    if (!version.is_number_unsigned()) { error = "this is not a combo file"; return false; }
    if (version > ComboVersion) { error = "the combo is from a newer version"; return false; }
    const auto text_ = [&](const char* key, std::string& out, std::size_t limit, bool needed) {
        const auto at = value.find(key);
        if (at == value.end()) return !needed;
        if (!at->is_string() || !CleanText(at->get<std::string>(), limit)) return false;
        out = at->get<std::string>();
        return !needed || !out.empty();
    };
    if (!text_("id", combo.id, 32, true) || !ValidComboId(combo.id)) { error = "the combo's id is not valid"; return false; }
    if (!text_("name", combo.name, ComboNameBytes, true)) { error = "the combo's name is missing or too long"; return false; }
    if (!text_("notation", combo.notation, ComboNotationBytes, false)) { error = "the combo's notation is not valid"; return false; }
    const auto number = [&](const char* key, std::int64_t low, std::int64_t high, std::int64_t& out) {
        const auto at = value.find(key);
        if (at == value.end() || !at->is_number_integer()) return false;
        out = at->get<std::int64_t>();
        return out >= low && out <= high;
    };
    std::int64_t fighter = 0, hits = 0, created = 0;
    if (!number("fighter", -1, 999, fighter) || !number("hits", 0, 999, hits) || !number("created", 0, std::int64_t(1) << 40, created)) { error = "a number in the combo is out of range"; return false; }
    combo.fighter = static_cast<int>(fighter); combo.hits = static_cast<int>(hits); combo.created = created;
    const auto start = value.value("start", nlohmann::json::object());
    const auto x = start.is_object() ? start.value("x", nlohmann::json()) : nlohmann::json();
    if (!x.is_array() || x.size() != 2) { error = "the start positions are missing"; return false; }
    for (int side = 0; side < 2; ++side) {
        if (!x[side].is_number()) { error = "the start positions are not numbers"; return false; }
        const double at = x[side].get<double>();
        if (!std::isfinite(at) || std::abs(at) > 100) { error = "a start position is out of range"; return false; }
        combo.x[side] = static_cast<float>(at);
    }
    const auto runs = value.find("frames");
    if (runs == value.end() || !runs->is_array() || runs->empty() || runs->size() > static_cast<std::size_t>(MaxFrames)) { error = "the combo has no frames or too many"; return false; }
    std::size_t total = 0;
    for (const auto& run : *runs) {
        if (!run.is_array() || run.size() != 3 || !run[0].is_number_unsigned() || !run[1].is_number_unsigned() || !run[2].is_number_unsigned()) { error = "a frame is not three numbers"; combo.frames.clear(); return false; }
        const std::uint64_t count = run[2].get<std::uint64_t>();
        if (count == 0 || count > static_cast<std::uint64_t>(MaxFrames) || total + count > static_cast<std::size_t>(MaxFrames)) { error = "the combo is too long"; combo.frames.clear(); return false; }
        total += static_cast<std::size_t>(count);
        const Input frame{run[0].get<unsigned>() & FightButtons, run[1].get<unsigned>() & FightButtons, 0, 0};
        combo.frames.insert(combo.frames.end(), static_cast<std::size_t>(count), frame);
    }
    return true;
}
} }
