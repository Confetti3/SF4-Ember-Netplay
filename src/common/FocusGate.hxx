#pragma once

// USF4 checks every frame whether its window is in front, and stops its
// sound and pads when it is not:
// - the app's frame (0x4040A0) asks GetForegroundWindow at 0x4042F8 and
//   mutes the master volume while the window is behind, restores it in front;
// - the pad update (0x512180) clears every keyboard, pad and player input;
// - the pad poll (0x6D8D70) skips XInput and DirectInput.
// Ember edits these three places (sf4e__BackgroundPlay.cxx): the frame reads
// the foreground window through Ember, and the two pad gates, which start
// with `test ebx, ebx` (85 DB), become short jumps whose decision Ember makes
// in C++. With Background play off each answers exactly as the game did.
#include <cstdint>
#include <cstring>

#include "CodeEdit.hxx"

namespace sf4e { namespace focus_gate {

// 0x5121AD: past the gate to 0x5121D5, which keeps the game's other check.
const std::uint8_t kUpdateSkip = 0x26;
// 0x6D8D8A: past the gate to 0x6D8DBE, the poll itself.
const std::uint8_t kPollSkip = 0x32;

// A gate's `test ebx, ebx`, turned into a short jump of `skip` bytes.
inline code_edit::Edit Gate(std::uint8_t* instruction, std::uint8_t skip) {
    return {instruction, 2, {0x85, 0xDB}, {0xEB, skip}};
}

// `call dword ptr [from]` (FF 15 from), turned into `call dword ptr [to]`.
inline code_edit::Edit CallThrough(std::uint8_t* instruction, std::uint32_t from, std::uint32_t to) {
    code_edit::Edit edit{instruction, 6, {0xFF, 0x15}, {0xFF, 0x15}};
    std::memcpy(edit.native + 2, &from, 4);
    std::memcpy(edit.patched + 2, &to, 4);
    return edit;
}

}}
