#pragma once
#include <cstdint>

namespace sf4e { namespace input {
// The game's pad provider codes, as Dimps::Pad::PadType numbers them, for code
// that cannot include Dimps. NetplayRuntime.cxx asserts that they agree.
enum PadKind : int { PadKeyboard = 1, PadXInput = 3, PadDirectInput = 4 };
// The physical buttons the native pad layer gives the same masks on every pad:
// the directions (bits 0 to 7 of the physical word are directions,
// MenuButtons.hxx, and the first four are the pad's, the same bits as the
// fight map's, ReplayInputs.hxx), and Back and Start, an Xbox pad's View and
// Start and a DirectInput pad's configured Select and Start (TrainingPad.hxx).
constexpr std::uint32_t PhysicalUp = 0x1, PhysicalDown = 0x2, PhysicalLeft = 0x4, PhysicalRight = 0x8,
    PhysicalBack = 0x100, PhysicalStart = 0x200;
// Physical XInput buttons as the native pad layer normalizes them (006D8415..006D8466).
namespace xinput {
constexpr std::uint32_t View = PhysicalBack, Start = PhysicalStart, Y = 0x10000, B = 0x20000, A = 0x40000, X = 0x80000,
    LB = 0x100000, RB = 0x200000, LT = 0x400000, RT = 0x800000, LS = 0x1000000, RS = 0x2000000;
}
} }
