#pragma once

namespace sf4e {
// The largest input delay a fighter can choose, in frames. Preferences, the
// room authority, its checkpoints and the connection check all share it.
constexpr int MaximumInputDelay = 10;

// Auto takes the connection check's recommendation, held between these.
constexpr int AutoInputDelayMinimum = 1;
constexpr int AutoInputDelayMaximum = 3;
// The delay Auto readies with. Without a recommendation (-1) it is two
// frames, where a chosen delay starts.
constexpr int AutoInputDelay(int recommended) {
    return recommended < 0 || recommended > MaximumInputDelay ? 2 :
        recommended < AutoInputDelayMinimum ? AutoInputDelayMinimum :
        recommended > AutoInputDelayMaximum ? AutoInputDelayMaximum : recommended;
}

// Where a delay command carries a number, this asks for Auto.
constexpr int AutoInputDelayChoice = -2;
// One step along the delay row: Auto, then 0 to MaximumInputDelay.
constexpr int StepInputDelay(bool automatic, int delay, int delta) {
    return automatic ? (delta > 0 ? 0 : AutoInputDelayChoice) :
        delay + delta < 0 ? AutoInputDelayChoice :
        delay + delta > MaximumInputDelay ? MaximumInputDelay : delay + delta;
}
}
