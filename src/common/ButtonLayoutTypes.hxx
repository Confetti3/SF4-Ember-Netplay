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
    return std::all_of(copies.begin(), copies.end(), [](int copy) { return copy >= -1 && copy < static_cast<int>(LayoutGlyphs.size()); });
}
inline bool ValidLayout(const ButtonLayout& layout) {
    return std::all_of(layout.begin(), layout.end(), [](std::uint32_t mask) { return !(mask & ~FightMask); });
}
// Ember's own actions a player binds to a layout button by pressing it on the
// button layout page: confirm and go back in Ember's menus, record and play in
// Training. Each has a partner that never shares its button. Start, View and
// the directions are never bound.
enum BoundAction : unsigned { MenuConfirmButton, MenuBackButton, TrainingRecordButton, TrainingPlayButton, BoundActionCount };
struct BoundActionInfo {
    // Its key in a saved layout.
    const char* json;
    BoundAction partner;
    // Training's buttons are read beside the fight, so one the layout gives an
    // attack would fight and record at once: it does not count. A menu button
    // the player leaves unbound is the game's own (input::ResolveMenuButtons).
    bool needsFreeButton;
};
constexpr std::array<BoundActionInfo, BoundActionCount> BoundActions = {{
    {"menuConfirm", MenuBackButton, false}, {"menuBack", MenuConfirmButton, false},
    {"trainingRecord", TrainingPlayButton, true}, {"trainingPlay", TrainingRecordButton, true}}};
// For each action, the layout button bound to it as an index into LayoutGlyphs; -1 for none.
using BoundButtons = std::array<int, BoundActionCount>;
constexpr BoundButtons NoBoundButtons = {-1, -1, -1, -1};
inline bool BoundIndexInRange(std::int64_t index) { return index >= -1 && index < static_cast<std::int64_t>(LayoutGlyphs.size()); }
inline bool ValidBoundButtons(const BoundButtons& bound) {
    for (unsigned action = 0; action < BoundActionCount; ++action) {
        const int button = bound[action];
        if (!BoundIndexInRange(button) || (button >= 0 && button == bound[BoundActions[action].partner])) return false;
    }
    return true;
}
// The physical mask of a layout button, 0 for none or a button the pad lacks.
inline std::uint32_t LayoutButtonMask(int index, const ListedButtons& has = AllLayoutButtons) {
    return index >= 0 && index < static_cast<int>(LayoutBits.size()) && has[index] ? std::uint32_t(1) << LayoutBits[index] : 0;
}
// Whether a layout button can act for an action under this table: always, but
// for an action that needs a button free of attacks.
inline bool BoundButtonWorks(BoundAction action, int index, const ButtonLayout& table) {
    return index >= 0 && index < static_cast<int>(table.size()) && (!BoundActions[action].needsFreeButton || !table[index]);
}
// The physical mask of the button bound to an action, 0 when none is or it does not work.
inline std::uint32_t BoundButtonMask(const BoundButtons& bound, BoundAction action, const ListedButtons& has, const ButtonLayout& table) {
    return BoundButtonWorks(action, bound[action], table) ? LayoutButtonMask(bound[action], has) : 0;
}
struct StoredLayout {
    Identity identity;
    ButtonLayout buttons = DefaultLayout;
    std::int64_t edited = 0;
    BoundButtons bound = NoBoundButtons;
    ExtraCopies extras = NoExtraCopies;
    bool operator==(const StoredLayout& other) const {
        return identity == other.identity && buttons == other.buttons && edited == other.edited && bound == other.bound && extras == other.extras;
    }
};
} }
