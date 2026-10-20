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
// The buttons the player chose to confirm and go back in Ember's menus, as
// indexes into LayoutGlyphs; -1 keeps what the game's own menus use
// (input::ResolveMenuButtons). Start, View and the directions are never bound.
struct MenuBinding {
    int confirm = -1, back = -1;
    bool operator==(const MenuBinding& other) const { return confirm == other.confirm && back == other.back; }
    bool operator!=(const MenuBinding& other) const { return !(*this == other); }
};
inline bool ValidMenuBinding(const MenuBinding& menu) {
    const auto inRange = [](int index) { return index >= -1 && index < static_cast<int>(LayoutGlyphs.size()); };
    return inRange(menu.confirm) && inRange(menu.back) && (menu.confirm < 0 || menu.confirm != menu.back);
}
// The physical mask of a bound index, 0 for none or a button the pad lacks.
inline std::uint32_t MenuBindingMask(int index, const ListedButtons& has = AllLayoutButtons) {
    return index >= 0 && index < static_cast<int>(LayoutBits.size()) && has[index] ? std::uint32_t(1) << LayoutBits[index] : 0;
}
struct StoredLayout {
    Identity identity;
    ButtonLayout buttons = DefaultLayout;
    std::int64_t edited = 0;
    MenuBinding menu;
    ExtraCopies extras = NoExtraCopies;
    bool operator==(const StoredLayout& other) const {
        return identity == other.identity && buttons == other.buttons && edited == other.edited && menu == other.menu && extras == other.extras;
    }
};
} }
