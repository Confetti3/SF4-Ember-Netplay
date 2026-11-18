#pragma once

namespace sf4e {
// Three limits are kept apart.
//
// Wire: the largest delay any snapshot, checkpoint or match record may hold.
// Earlier builds let a fighter choose up to 10, so stored data from them
// still loads, and a committed match plays at the delay it recorded.
constexpr int InputDelayWireCeiling = 10;
// Choice: the largest input delay a fighter can choose now, in frames.
// Anything higher is unplayable. Preferences, the room authority when it
// records Ready and the connection check all share it.
constexpr int MaximumInputDelay = 5;
static_assert(MaximumInputDelay <= InputDelayWireCeiling, "a choice always fits the wire");
// The smallest. 0 adds no delay: the connection check never recommends it,
// and Auto readies with it only in its Responsive style.
constexpr int MinimumInputDelay = 0;
// Where a new profile's chosen delay starts, and what a saved delay that is
// not a whole number from 0 to InputDelayWireCeiling loads as
// (InputDelayPreference.hxx).
constexpr int DefaultInputDelay = 2;
// The connection check recommends from this to MaximumInputDelay
// (recovery.rs: recommended_delay); a 0 from it is no recommendation.
constexpr int RecommendedInputDelayMinimum = 1;

// Auto takes the connection check's recommendation, held between these.
constexpr int AutoInputDelayMinimum = 1;
constexpr int AutoInputDelayMaximum = 3;
static_assert(AutoInputDelayMaximum <= MaximumInputDelay, "Auto stays within the choice");
static_assert(AutoInputDelayMinimum >= RecommendedInputDelayMinimum, "Balanced Auto never readies at 0 frames");
// How Auto turns the recommendation into a delay, the player's choice:
// Balanced takes it within AutoInputDelayMinimum to AutoInputDelayMaximum;
// Responsive one frame less, from 0 to 2, so a close opponent plays at 0 and
// rolls back more; Smooth one frame more, from 1 to 4, for fewer rollbacks.
enum class AutoDelayStyle : int { Balanced = 0, Responsive, Smooth };
constexpr int AutoDelayStyles = 3;
struct AutoDelayRange { int minimum, maximum, shift; };
constexpr AutoDelayRange AutoInputDelayRange(AutoDelayStyle style) {
    return style == AutoDelayStyle::Responsive ? AutoDelayRange{MinimumInputDelay, 2, -1} :
        style == AutoDelayStyle::Smooth ? AutoDelayRange{AutoInputDelayMinimum, 4, 1} :
        AutoDelayRange{AutoInputDelayMinimum, AutoInputDelayMaximum, 0};
}
static_assert(AutoInputDelayRange(AutoDelayStyle::Smooth).maximum <= MaximumInputDelay, "Auto stays within the choice");
// The delay Auto readies with. Without a recommendation (-1) it is the
// default, where a chosen delay starts, in every style.
constexpr int ClampAutoDelay(int delay, AutoDelayRange range) {
    return delay < range.minimum ? range.minimum : delay > range.maximum ? range.maximum : delay;
}
constexpr int AutoInputDelay(int recommended, AutoDelayStyle style = AutoDelayStyle::Balanced) {
    return recommended < 0 || recommended > InputDelayWireCeiling ? DefaultInputDelay :
        ClampAutoDelay(recommended + AutoInputDelayRange(style).shift, AutoInputDelayRange(style));
}

// A measured round trip as the bands read it: its median and its 95th
// percentile, in whole milliseconds.
struct RoundTrip { int medianMs, p95Ms; };
// A round trip in microseconds, as the connection check reports it, in the
// whole milliseconds the bands read, with half a millisecond rounding up
// (recovery.rs: delay_band): 80.499 ms is 80, 80.500 ms is 81. Anything past
// a million milliseconds is one, far beyond the last band.
constexpr int RoundTripMs(std::uint64_t us) {
    return us >= 1000000000ULL ? 1000000 : static_cast<int>(us / 1000 + (us % 1000 >= 500 ? 1 : 0));
}

// Where Auto's recommendation steps up, by round trip: one frame through
// firstBandMs, then one more frame per bandStepMs, up to MaximumInputDelay.
// The rollback a recommendation leaves is the game's to judge, since games show
// a late input differently, so each game hands its own (GameAdapter.hxx:
// Adapter::delayBands), and its connection checks and played games both read
// it. The styles below hold every game's recommendation alike.
struct InputDelayBands { int firstBandMs, bandStepMs; };
// SF4's, from its players' experience, and the connection check's own
// (recovery.rs: delay_band). The helper still recommends with these, and the
// room takes its recommendation only as the sign that the check measured
// (IrohRoom::ProbeSnapshot::measured); the game's delay comes from its own
// bands. The edges are pinned by the same table in both places' tests.
constexpr InputDelayBands Sf4InputDelayBands = {80, 70};
constexpr int InputDelayFramesFor(InputDelayBands bands, int rttMs) {
    return rttMs <= bands.firstBandMs ? RecommendedInputDelayMinimum :
        RecommendedInputDelayMinimum + (rttMs - bands.firstBandMs + bands.bandStepMs - 1) / bands.bandStepMs;
}
constexpr int InputDelayBand(InputDelayBands bands, int rttMs) {
    return InputDelayFramesFor(bands, rttMs) > MaximumInputDelay ? MaximumInputDelay : InputDelayFramesFor(bands, rttMs);
}
// The recommendation for a round trip: the median's band, raised to one below
// the tail's when the tail is more than one band worse (recovery.rs:
// recommended_delay).
constexpr int RecommendedInputDelay(InputDelayBands bands, RoundTrip rtt) {
    return InputDelayBand(bands, rtt.medianMs) > InputDelayBand(bands, rtt.p95Ms) - 1 ? InputDelayBand(bands, rtt.medianMs) :
        InputDelayBand(bands, rtt.p95Ms) - 1;
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
