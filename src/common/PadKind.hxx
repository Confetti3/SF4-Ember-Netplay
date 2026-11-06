#pragma once
#include <cstdint>

namespace sf4e { namespace input {
// The game's pad provider codes, as Dimps::Pad::PadType numbers them, for code
// that cannot include Dimps. NetplayRuntime.cxx asserts that they agree.
enum PadKind : int { PadKeyboard = 1, PadXInput = 3, PadDirectInput = 4 };
// Physical XInput buttons as the native pad layer normalizes them (006D8415..006D8466).
namespace xinput {
constexpr std::uint32_t View = 0x100, Start = 0x200, Y = 0x10000, B = 0x20000, A = 0x40000, X = 0x80000,
    LB = 0x100000, RB = 0x200000, LT = 0x400000, RT = 0x800000, LS = 0x1000000, RS = 0x2000000;
// An XInput pad's buttons as XInputGetState gives them (XINPUT_GAMEPAD's
// wButtons) in the layout above, for a game whose pads Ember reads through
// XInput itself. The D-pad is not among them (it is the directions), nor are
// the triggers, which XInput reports as axes.
constexpr std::uint32_t Physical(std::uint16_t buttons) {
    return ((buttons & 0x0010) ? Start : 0) | ((buttons & 0x0020) ? View : 0) | ((buttons & 0x0040) ? LS : 0) |
        ((buttons & 0x0080) ? RS : 0) | ((buttons & 0x0100) ? LB : 0) | ((buttons & 0x0200) ? RB : 0) |
        ((buttons & 0x1000) ? A : 0) | ((buttons & 0x2000) ? B : 0) | ((buttons & 0x4000) ? X : 0) |
        ((buttons & 0x8000) ? Y : 0);
}
}
} }
