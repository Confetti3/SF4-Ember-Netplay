#pragma once
#include "GamepadIdentity.hxx"
#include "ExtraButtons.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace sf4e { namespace gamepads {
using ButtonLayout = std::array<std::uint32_t, 10>;
using ListedButtons = std::array<bool, 10>;
using FightNames = std::array<std::string, 10>;
constexpr ListedButtons AllLayoutButtons = {true, true, true, true, true, true, true, true, true, true};
constexpr std::array<const char*, 10> LayoutGlyphs = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS"};
constexpr std::array<unsigned, 10> LayoutBits = {18, 17, 19, 16, 20, 21, 22, 23, 24, 25};
constexpr std::array<std::uint32_t, 6> AttackMasks = {0x10, 0x20, 0x400, 0x40, 0x80, 0x800};
constexpr std::array<const char*, 6> AttackNames = {"LP", "MP", "HP", "LK", "MK", "HK"};
constexpr std::uint32_t FightMask = 0xCF0;
constexpr ButtonLayout DefaultLayout = {0x40, 0x80, 0x10, 0x20, 0x430, 0x400, 0x8C0, 0x800, 0, 0};
inline bool ValidExtraCopies(const ExtraCopies& copies) {
    return std::all_of(copies.begin(), copies.end(), [](int copy) { return copy >= -1 && copy < CopyTargetCount; });
}
// The glyph id of what an extra button copies (ExtraCopies), as held glyphs name it.
inline const char* CopyTargetGlyph(int copy) { return copy == CopiesView ? "View" : copy == CopiesStart ? "Start" : LayoutGlyphs[copy]; }
inline bool ValidLayout(const ButtonLayout& layout) {
    return std::all_of(layout.begin(), layout.end(), [](std::uint32_t mask) { return !(mask & ~FightMask); });
}
// Ember's own actions a player binds to a layout button by pressing it on the
// button layout page: confirm and go back in Ember's menus; record, play and
// the controls' own button in Training. The actions of one group never share
// a button. Start, View and the directions are never bound.
enum BoundAction : unsigned { MenuConfirmButton, MenuBackButton, TrainingRecordButton, TrainingPlayButton, TrainingControlsButton, BoundActionCount };
enum class BoundGroup : unsigned { Menu, Training, Count };
// Automatic, a binding only Training's controls button holds by default
// (BoundActionInfo::unset): it is R3 (the right stick click, as the controls
// always opened) while the layout gives R3 no fight function and no other
// Training action has it, else off (ResolveBound). A button or -1 (off) the
// player chooses is explicit and always wins.
constexpr int BoundAuto = -2, AutoControlsButton = 9;
// Each action's key in a saved layout, its group, and its binding when the
// layout holds none.
struct BoundActionInfo { const char* json; BoundGroup group; int unset; };
constexpr std::array<BoundActionInfo, BoundActionCount> BoundActions = {{
    {"menuConfirm", BoundGroup::Menu, -1}, {"menuBack", BoundGroup::Menu, -1}, {"trainingRecord", BoundGroup::Training, -1},
    {"trainingPlay", BoundGroup::Training, -1}, {"trainingControls", BoundGroup::Training, BoundAuto}}};
// For each action, the layout button bound to it as an index into LayoutGlyphs;
// -1 for none (for a menu action: A and B, input::ResolveMenuButtons).
using BoundButtons = std::array<int, BoundActionCount>;
constexpr BoundButtons DefaultBoundButtons = {-1, -1, -1, -1, BoundAuto};
inline bool BoundIndexInRange(std::int64_t index) { return index >= -1 && index < static_cast<std::int64_t>(LayoutGlyphs.size()); }
// Whether a binding is a layout button, off, or automatic for an action that defaults to it.
inline bool BoundValueAllowed(unsigned action, std::int64_t value) {
    return BoundIndexInRange(value) || (value == BoundAuto && BoundActions[action].unset == BoundAuto);
}
// Whether one group's bindings are in range and share no button.
inline bool ValidBoundGroup(const BoundButtons& bound, BoundGroup group) {
    for (unsigned action = 0; action < BoundActionCount; ++action) {
        if (BoundActions[action].group != group) continue;
        if (!BoundValueAllowed(action, bound[action])) return false;
        for (unsigned other = action + 1; other < BoundActionCount; ++other)
            if (BoundActions[other].group == group && bound[action] >= 0 && bound[action] == bound[other]) return false;
    }
    return true;
}
inline bool ValidBoundButtons(const BoundButtons& bound) {
    for (unsigned group = 0; group < static_cast<unsigned>(BoundGroup::Count); ++group)
        if (!ValidBoundGroup(bound, static_cast<BoundGroup>(group))) return false;
    return true;
}
// The physical mask of a layout button, 0 for none or a button the pad lacks.
inline std::uint32_t LayoutButtonMask(int index, const ListedButtons& has = AllLayoutButtons) {
    return index >= 0 && index < static_cast<int>(LayoutBits.size()) && has[index] ? std::uint32_t(1) << LayoutBits[index] : 0;
}
// The bindings with automatic resolved against the layout's fight table: R3
// for the controls while R3 attacks nothing and Record and Play are not on
// it, else off. Everything else is as stored.
inline BoundButtons ResolveBound(BoundButtons bound, const ButtonLayout& table) {
    if (bound[TrainingControlsButton] == BoundAuto) {
        const bool free = table[AutoControlsButton] == 0 && bound[TrainingRecordButton] != AutoControlsButton && bound[TrainingPlayButton] != AutoControlsButton;
        bound[TrainingControlsButton] = free ? AutoControlsButton : -1;
    }
    return bound;
}
// The fight table offline Training plays with: Training's own buttons (record,
// play and the controls', automatic resolved) do nothing in the fight there,
// so a press records, plays or opens the controls and never attacks too, and
// the game never takes it. Everywhere else they keep what the layout gives
// them. Automatic never takes an attack away; a player who chose the controls
// button on an attack gets what they chose.
inline ButtonLayout TrainingTable(ButtonLayout table, const BoundButtons& stored) {
    const auto bound = ResolveBound(stored, table);
    for (const BoundAction action : {TrainingRecordButton, TrainingPlayButton, TrainingControlsButton})
        if (bound[action] >= 0 && bound[action] < static_cast<int>(table.size())) table[bound[action]] = 0;
    return table;
}
struct StoredLayout {
    Identity identity;
    ButtonLayout buttons = DefaultLayout;
    std::int64_t edited = 0;
    BoundButtons bound = DefaultBoundButtons;
    ExtraCopies extras = NoExtraCopies;
    bool operator==(const StoredLayout& other) const {
        return identity == other.identity && buttons == other.buttons && edited == other.edited && bound == other.bound && extras == other.extras;
    }
};
} }
