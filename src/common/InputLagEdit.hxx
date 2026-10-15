#pragma once

// Lower input lag (Player & controller, experimental; issue #77). Each battle
// frame, the battle's network unit (Network::Unit, task "NET MAIN") stores
// both fighters' inputs in a ring under one frame number and reads the inputs
// to play under another (0x5CFB80). Offline it stores one frame ahead of the
// one it reads (0x5CFA20 starts the counters at 0 and 1), so every input
// reaches the fighters a frame late. Storing under the frame being read
// removes that frame. The whole unit is in the battle memento, so rollback
// keeps it; every PC simulating a match must run the same bytes for the whole
// battle. The replay recorder (0x59AA20) takes the unit's output, after the
// ring, and playback hands the recorded inputs straight to the fighters, so a
// replay plays the same with or without the edit.
#include <cstdint>

#include "CodeEdit.hxx"

namespace sf4e { namespace input_lag {

// 0x5CFBB6: `mov edx, [esi+2Ch]` (the frame to store under) becomes
// `mov edx, [esi+28h]` (the frame being read).
inline code_edit::Edit StoreAtReadFrame(std::uint8_t* instruction) {
    return {instruction, 3, {0x8B, 0x56, 0x2C}, {0x8B, 0x56, 0x28}};
}

}}
