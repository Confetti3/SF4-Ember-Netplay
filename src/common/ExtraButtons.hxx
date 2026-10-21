#pragma once
#include <array>

namespace sf4e { namespace gamepads {
// Buttons XInput has no bit for. Each can copy one of the ten layout buttons, View or Start.
// Paddles follow SDL: the first paddle under each hand is the upper one (Xbox Elite P1 and P3).
enum class Extra { Touchpad, Home, Share, RightPaddle1, LeftPaddle1, RightPaddle2, LeftPaddle2, Count };
constexpr unsigned ExtraCount = static_cast<unsigned>(Extra::Count);
// What an extra button copies: a layout button (an index into LayoutGlyphs:
// A, B, X, Y, LB, RB, LT, RT, LS, RS), View or Start after them; -1 for none.
constexpr int CopiesView = 10, CopiesStart = 11, CopyTargetCount = 12;
// For each extra button, what it copies.
using ExtraCopies = std::array<int, ExtraCount>;
constexpr ExtraCopies NoExtraCopies = {-1, -1, -1, -1, -1, -1, -1};
// The extra buttons' ids, in Extra order, as held glyphs and row ids name them.
constexpr std::array<const char*, ExtraCount> ExtraIds = {"touchpad", "home", "share", "right-paddle1", "left-paddle1", "right-paddle2", "left-paddle2"};
using ListedExtras = std::array<bool, ExtraCount>;
} }
