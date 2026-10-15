#pragma once

namespace sf4e {
// The largest input delay a fighter can choose, in frames. Preferences, the
// room authority, its checkpoints and the connection check all share it.
constexpr int MaximumInputDelay = 10;
// The smallest. 0 adds no delay and is only ever a player's own choice: the
// connection check never recommends it and Auto never readies with it.
constexpr int MinimumInputDelay = 0;
// Where a new profile's chosen delay starts, and what a saved delay that is
// not a whole number in range loads as (InputDelayPreference.hxx).
constexpr int DefaultInputDelay = 2;
// The connection check recommends from this to MaximumInputDelay
// (recovery.rs: recommended_delay); a 0 from it is no recommendation.
constexpr int RecommendedInputDelayMinimum = 1;

// Auto takes the connection check's recommendation, held between these.
constexpr int AutoInputDelayMinimum = 1;
constexpr int AutoInputDelayMaximum = 4;
static_assert(AutoInputDelayMinimum >= RecommendedInputDelayMinimum, "Auto never readies at 0 frames");
// The delay Auto readies with. Without a recommendation (-1) it is the
// default, where a chosen delay starts.
constexpr int AutoInputDelay(int recommended) {
    return recommended < 0 || recommended > MaximumInputDelay ? DefaultInputDelay :
        recommended < AutoInputDelayMinimum ? AutoInputDelayMinimum :
        recommended > AutoInputDelayMaximum ? AutoInputDelayMaximum : recommended;
}

// The delay a round trip of `rttMs` asks for: one frame through 80 ms, then
// one more frame per 70 ms, up to MaximumInputDelay. This is the connection
// check's band function in whole milliseconds (recovery.rs: delay_band). The
// game runs apart from the helper that owns that function, so the edges are
// kept in both places and pinned by the same table in each one's tests.
constexpr int InputDelayFirstBandMs = 80;
constexpr int InputDelayBandStepMs = 70;
constexpr int InputDelayFramesFor(int rttMs) {
    return rttMs <= InputDelayFirstBandMs ? RecommendedInputDelayMinimum :
        RecommendedInputDelayMinimum + (rttMs - InputDelayFirstBandMs + InputDelayBandStepMs - 1) / InputDelayBandStepMs;
}
constexpr int InputDelayBand(int rttMs) {
    return InputDelayFramesFor(rttMs) > MaximumInputDelay ? MaximumInputDelay : InputDelayFramesFor(rttMs);
}
// The recommendation for a median and a 95th percentile round trip: the
// median's band, raised to one below the tail's when the tail is more than
// one band worse (recovery.rs: recommended_delay).
constexpr int RecommendedInputDelay(int medianMs, int p95Ms) {
    return InputDelayBand(medianMs) > InputDelayBand(p95Ms) - 1 ? InputDelayBand(medianMs) : InputDelayBand(p95Ms) - 1;
}

// Where a delay command carries a number, this asks for Auto. It is never
// stored as a delay: preferences keep Auto in its own autoInputDelay flag.
constexpr int AutoInputDelayChoice = -2;
// One step along the delay row: Auto, then MinimumInputDelay to MaximumInputDelay.
constexpr int StepInputDelay(bool automatic, int delay, int delta) {
    return automatic ? (delta > 0 ? MinimumInputDelay : AutoInputDelayChoice) :
        delay + delta < MinimumInputDelay ? AutoInputDelayChoice :
        delay + delta > MaximumInputDelay ? MaximumInputDelay : delay + delta;
}
}
