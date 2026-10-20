#pragma once
#include "ButtonLayoutTypes.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace gamepads {
struct LayoutPreset { const char* id; const char* key; const char* detailKey; ButtonLayout buttons; };
constexpr std::array<LayoutPreset, 3> LayoutPresets = {{
    {"default", "button_layout.preset.default", "button_layout.preset.default_detail", DefaultLayout},
    {"triggers", "button_layout.preset.triggers", "button_layout.preset.triggers_detail", {0x40, 0x80, 0x10, 0x20, 0x800, 0x400, 0x430, 0x8C0, 0, 0}},
    {"focus-throw", "button_layout.preset.focus_throw", "button_layout.preset.focus_throw_detail", {0x40, 0x80, 0x10, 0x20, 0xA0, 0x400, 0x50, 0x800, 0x430, 0x8C0}}
}};
inline const LayoutPreset* PresetOf(const ButtonLayout& layout) {
    for (const auto& preset : LayoutPresets) if (preset.buttons == layout) return &preset;
    return nullptr;
}
inline std::string FightName(std::uint32_t mask) {
    if (mask == 0x430) return "3P";
    if (mask == 0x8C0) return "3K";
    std::string name;
    for (unsigned i = 0; i < AttackMasks.size(); ++i) if (mask & AttackMasks[i]) {
        if (!name.empty()) name += "+";
        name += AttackNames[i];
    }
    return name;
}
inline FightNames NamesOf(const ButtonLayout& layout) {
    FightNames names;
    std::transform(layout.begin(), layout.end(), names.begin(), FightName);
    return names;
}
struct LayoutAssignment { bool changed = false; std::optional<unsigned> swapped; std::uint32_t old = 0; };
inline LayoutAssignment Assign(ButtonLayout& layout, unsigned button, std::uint32_t mask,
    const ListedButtons& listed = AllLayoutButtons) {
    if (button >= layout.size() || !listed[button] || (mask & ~FightMask) || layout[button] == mask) return {};
    LayoutAssignment result{true, {}, layout[button]};
    for (unsigned i = 0; i < layout.size(); ++i) if (i != button && listed[i] && layout[i] == mask) {
        if (!result.swapped) result.swapped = i;
        layout[i] = result.old;
    }
    layout[button] = mask;
    return result;
}
inline bool SetMask(ButtonLayout& layout, unsigned button, std::uint32_t mask) {
    if (button >= layout.size() || (mask & ~FightMask) || layout[button] == mask) return false;
    layout[button] = mask;
    return true;
}
// The layout button an extra button copies, if the pad has that button.
inline std::optional<unsigned> CopiedButton(const ExtraCopies& copies, unsigned extra, const ListedButtons& has) {
    const int copy = copies[extra];
    if (copy < 0 || copy >= static_cast<int>(has.size()) || !has[copy]) return {};
    return static_cast<unsigned>(copy);
}
inline std::vector<std::uint32_t> MissingAttacks(const ButtonLayout& layout, const ListedButtons& listed) {
    std::vector<std::uint32_t> missing;
    for (auto attack : AttackMasks) {
        bool found = false;
        for (unsigned i = 0; i < layout.size(); ++i) if (listed[i] && layout[i] == attack) found = true;
        if (!found) missing.push_back(attack);
    }
    return missing;
}
struct LayoutMatch { std::optional<unsigned> index; bool byGuid = false; };
inline LayoutMatch FindLayout(const std::vector<StoredLayout>& layouts, const Identity& pad,
    const std::vector<Identity>& connected) {
    for (unsigned i = 0; i < layouts.size(); ++i) if (Same(layouts[i].identity, pad)) return {i, false};
    if (!pad.serial.empty()) return {};
    const auto sameGuid = std::count_if(connected.begin(), connected.end(), [&](const Identity& other) { return other.guid == pad.guid; });
    const bool targetConnected = std::any_of(connected.begin(), connected.end(), [&](const Identity& other) { return Same(other, pad); });
    if (sameGuid > (targetConnected ? 1 : 0)) return {};
    LayoutMatch result;
    for (unsigned i = 0; i < layouts.size(); ++i) if (layouts[i].identity.serial.empty() && layouts[i].identity.guid == pad.guid) {
        if (result.index) return {};
        result = {i, true};
    }
    return result;
}
inline std::optional<Identity> UpsertLayout(std::vector<StoredLayout>& layouts, StoredLayout value) {
    for (auto& layout : layouts) if (Same(layout.identity, value.identity)) { layout = std::move(value); return {}; }
    layouts.push_back(std::move(value));
    if (layouts.size() <= 32) return {};
    const auto oldest = std::min_element(layouts.begin(), layouts.end(), [](const StoredLayout& a, const StoredLayout& b) {
        return a.edited < b.edited;
    });
    auto dropped = oldest->identity;
    layouts.erase(oldest);
    return dropped;
}
inline void RemoveLayout(std::vector<StoredLayout>& layouts, const Identity& pad) {
    layouts.erase(std::remove_if(layouts.begin(), layouts.end(), [&](const StoredLayout& layout) {
        return Same(layout.identity, pad);
    }), layouts.end());
}
inline std::string LayoutDescription(const ButtonLayout& layout) {
    std::string text;
    for (unsigned i = 0; i < layout.size(); ++i) {
        if (i) text += " ";
        text += std::string(LayoutGlyphs[i]) + "=" + (layout[i] ? FightName(layout[i]) : "-");
    }
    const auto* preset = PresetOf(layout);
    return text + " " + (preset ? preset->id : "custom");
}
} }
