#pragma once

namespace sf4e {
// The largest input delay a fighter can choose, in frames. Preferences, the
// room authority, its checkpoints and the connection check all share it.
constexpr int MaximumInputDelay = 10;
// The smallest. A match at 0 frames crashes, so nothing offers it, and a 0
// that still arrives (a saved preference, an older client's Ready, a
// checkpoint) plays at this instead.
constexpr int MinimumInputDelay = 1;
constexpr int PlayableInputDelay(int delay) { return delay < MinimumInputDelay ? MinimumInputDelay : delay; }
// A saved delay as the game uses it: out of range is the default of 2, and a
// 0 saved before 0 was withdrawn becomes 1.
constexpr int SavedInputDelay(int delay) { return delay < 0 || delay > MaximumInputDelay ? 2 : PlayableInputDelay(delay); }

// Auto takes the connection check's recommendation, held between these.
constexpr int AutoInputDelayMinimum = 1;
constexpr int AutoInputDelayMaximum = 3;
static_assert(AutoInputDelayMinimum >= MinimumInputDelay, "Auto never readies below the smallest delay");
// The delay Auto readies with. Without a recommendation (-1) it is two
// frames, where a chosen delay starts.
constexpr int AutoInputDelay(int recommended) {
    return recommended < 0 || recommended > MaximumInputDelay ? 2 :
        recommended < AutoInputDelayMinimum ? AutoInputDelayMinimum :
        recommended > AutoInputDelayMaximum ? AutoInputDelayMaximum : recommended;
}

// Where a delay command carries a number, this asks for Auto. It is never
// stored as a delay: preferences keep Auto in its own autoInputDelay flag.
constexpr int AutoInputDelayChoice = -2;
// One step along the delay row: Auto, then MinimumInputDelay to MaximumInputDelay.
// A delay below the minimum (an older 0) steps as the minimum does.
constexpr int StepInputDelay(bool automatic, int delay, int delta) {
    return automatic ? (delta > 0 ? MinimumInputDelay : AutoInputDelayChoice) :
        PlayableInputDelay(delay) + delta < MinimumInputDelay ? AutoInputDelayChoice :
        PlayableInputDelay(delay) + delta > MaximumInputDelay ? MaximumInputDelay : PlayableInputDelay(delay) + delta;
}
}
