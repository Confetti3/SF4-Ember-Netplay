#pragma once
#include "PadKind.hxx"
#include <cstdint>
#include <string>

namespace sf4e { namespace input {
// The functions the game's own menus confirm and go back with: Pad::System
// +0xCC and +0xD0, which its constructor (0x517000) sets to LK and MK.
constexpr std::uint32_t MenuConfirmFunction = 0x40, MenuBackFunction = 0x80;
// The fight functions a stick's tab shortcuts are named by: the ends of its top row.
constexpr std::uint32_t LightPunchFunction = 0x10, HeavyPunchFunction = 0x400;
// The physical buttons (bits of the raw button word) that confirm and go back
// in Ember's menus for one device, and the prompt glyph naming each. Glyphs
// are string literals. The default is the game's generic one: bits 18 and 17,
// A and B on an XInput pad, buttons 3 and 2 on a DirectInput stick.
struct MenuButtons {
    std::uint32_t confirm = xinput::A, back = xinput::B;
    const char* confirmGlyph = "A";
    const char* backGlyph = "B";
    // The physical buttons whose fight function includes LP, and HP: a
    // stick's tab shortcuts (ui::MenuShortcuts), read from the fight table the
    // game maps its buttons through. The default is the generic layout,
    // buttons 4 and 6. Scalars, not an array: MSVC fails on brace-initialized
    // values of a type with an array member initializer.
    std::uint32_t lightPunch = 1u << 19, heavyPunch = 1u << 21;
    // The physical buttons holding fight function `function` (LP or HP), 0 for another.
    std::uint32_t Holding(std::uint32_t function) const {
        return function == LightPunchFunction ? lightPunch : function == HeavyPunchFunction ? heavyPunch : 0;
    }
};
// The physical buttons that start a Training recording and play a slot for one
// device (the player's TrainingRecordButton and TrainingPlayButton), 0 for none,
// and the names the pad gives them, empty for none.
struct TrainingButtons {
    std::uint32_t record = 0, play = 0;
    std::string recordName, playName;
};
// The glyph id of one physical XInput button, or null.
inline const char* XInputButtonName(std::uint32_t mask) {
    switch (mask) {
    case xinput::A: return "A"; case xinput::B: return "B"; case xinput::X: return "X"; case xinput::Y: return "Y";
    case xinput::LB: return "LB"; case xinput::RB: return "RB"; case xinput::LT: return "LT"; case xinput::RT: return "RT";
    case xinput::LS: return "LS"; case xinput::RS: return "RS"; case xinput::View: return "View"; case xinput::Start: return "Start";
    default: return nullptr;
    }
}
// Fight notation for a menu function, the prompt of a button whose shape Ember cannot know.
inline const char* MenuFunctionName(std::uint32_t function) {
    switch (function) {
    case 0x10: return "LP"; case 0x20: return "MP"; case 0x400: return "HP";
    case 0x40: return "LK"; case 0x80: return "MK"; case 0x800: return "HK";
    default: return nullptr;
    }
}
// The lowest physical bit from 8 up (bits 0 to 7 are directions) whose entry
// is exactly function, as the game's own prompt lookup 0x519D90 matches; 0 for none.
inline std::uint32_t PhysicalFunction(std::uint32_t function, const std::uint32_t* table) {
    if (table) for (unsigned bit = 8; bit < 32; ++bit) if (table[bit] == function) return std::uint32_t(1) << bit;
    return 0;
}
// Every physical bit from 8 up whose entry includes function (a 3P button holds LP).
inline std::uint32_t PhysicalHolding(std::uint32_t function, const std::uint32_t* table) {
    std::uint32_t bits = 0;
    for (unsigned bit = 8; bit < 32; ++bit) if (table[bit] & function) bits |= std::uint32_t(1) << bit;
    return bits;
}
// The one answer to "which buttons confirm and go back for this device".
// menu and fight are the device's binding record tables (+0x1FC and +0x17C),
// null when the record fails its bounds check and for an XInput pad. In
// order: the player's binding (boundConfirm, boundBack: a physical mask, 0 for
// none), the button holding the function in the game's menu table, the one
// holding it in the fight table, then bits 18 and 17. A zero function reads
// as the game's default. The LP and HP buttons are the fight table's, else
// the generic layout's.
inline MenuButtons ResolveMenuButtons(int deviceType, const std::uint32_t* menu, const std::uint32_t* fight,
    std::uint32_t confirmFunction = MenuConfirmFunction, std::uint32_t backFunction = MenuBackFunction,
    std::uint32_t boundConfirm = 0, std::uint32_t boundBack = 0) {
    if (!confirmFunction) confirmFunction = MenuConfirmFunction;
    if (!backFunction) backFunction = MenuBackFunction;
    const auto game = [&](std::uint32_t function, std::uint32_t fallback) {
        if (const auto bit = PhysicalFunction(function, menu)) return bit;
        if (const auto bit = PhysicalFunction(function, fight)) return bit;
        return fallback;
    };
    const auto gameConfirm = game(confirmFunction, xinput::A), gameBack = game(backFunction, xinput::B);
    // Both bound to one button is never stored; read it as no binding.
    if (boundConfirm && boundConfirm == boundBack) boundConfirm = boundBack = 0;
    MenuButtons result;
    result.confirm = boundConfirm ? boundConfirm : gameConfirm;
    result.back = boundBack ? boundBack : gameBack;
    // One bound to the other's default button (back bound to A, say): the
    // other takes this one's default, so the two never share one.
    if (result.confirm == result.back) {
        if (boundConfirm) result.back = gameConfirm; else result.confirm = gameBack;
        if (result.confirm == result.back) { result.confirm = gameConfirm; result.back = gameBack; }
        if (result.confirm == result.back) { result.confirm = xinput::A; result.back = xinput::B; }
    }
    const auto name = [&](std::uint32_t mask, std::uint32_t function, const char* fallback) {
        const char* glyph = deviceType == PadXInput ? XInputButtonName(mask) : nullptr;
        if (!glyph) glyph = MenuFunctionName(function);
        return glyph ? glyph : fallback;
    };
    result.confirmGlyph = name(result.confirm, confirmFunction, "LK");
    result.backGlyph = name(result.back, backFunction, "MK");
    if (fight) {
        result.lightPunch = PhysicalHolding(LightPunchFunction, fight);
        result.heavyPunch = PhysicalHolding(HeavyPunchFunction, fight);
    }
    return result;
}
} }
