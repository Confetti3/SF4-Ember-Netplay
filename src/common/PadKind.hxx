#pragma once
#include <cstdint>

namespace sf4e { namespace input {
// The game's pad provider codes, as Dimps::Pad::PadType numbers them, for code
// that cannot include Dimps. NetplayRuntime.cxx asserts that they agree.
enum PadKind : int { PadKeyboard = 1, PadXInput = 3, PadDirectInput = 4 };
// Physical XInput buttons as the native pad layer normalizes them (006D8415..006D8466).
namespace xinput {
constexpr std::uint32_t View = 0x100, Start = 0x200, Y = 0x10000, B = 0x20000, A = 0x40000, X = 0x80000,
    LB = 0x100000, RB = 0x200000, LT = 0x400000, RT = 0x800000;
}
} }
