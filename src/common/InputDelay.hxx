#pragma once

namespace sf4e {
// The largest input delay a fighter can choose, in frames. Preferences, the
// room authority, its checkpoints and the connection check all share it.
constexpr int MaximumInputDelay = 10;
// The smallest. A match at 0 frames crashes, so nothing offers it, and a 0
// that still arrives (a saved preference, an older client's Ready, a
// checkpoint) plays at this instead.
constexpr int MinimumInputDelay = 1;
inline int PlayableInputDelay(int delay) { return delay < MinimumInputDelay ? MinimumInputDelay : delay; }
// A saved delay as the game uses it: out of range is the default of 2, and a
// 0 saved before 0 was withdrawn becomes 1.
inline int SavedInputDelay(int delay) { return delay < 0 || delay > MaximumInputDelay ? 2 : PlayableInputDelay(delay); }
}
