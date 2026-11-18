#include "../common/InputDelay.hxx"
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include "test_support.hxx"
int main() {
    using namespace sf4e;
    // The three limits: a choice fits the wire, and Auto fits the choice.
    static_assert(InputDelayWireCeiling == 10 && MaximumInputDelay == 5 && AutoInputDelayMaximum == 3, "the limits");
    static_assert(MinimumInputDelay <= AutoInputDelayMinimum && AutoInputDelayMaximum <= MaximumInputDelay &&
        MaximumInputDelay <= InputDelayWireCeiling, "nested ranges");
    // Auto holds the recommendation between its bounds (1 to 3) and never readies at 0.
    CHECK(AutoInputDelay(0) == 1 && AutoInputDelay(1) == 1 && AutoInputDelay(2) == 2 && AutoInputDelay(3) == 3);
    CHECK(AutoInputDelay(4) == 3 && AutoInputDelay(5) == 3 && AutoInputDelay(MaximumInputDelay) == AutoInputDelayMaximum);
    for (int recommended = -1; recommended <= InputDelayWireCeiling + 1; ++recommended)
        CHECK(AutoInputDelay(recommended) >= AutoInputDelayMinimum && AutoInputDelay(recommended) <= AutoInputDelayMaximum &&
            AutoInputDelay(recommended) > 0);
    // No recommendation, or one outside the wire range, is a new profile's two frames.
    CHECK(AutoInputDelay(-1) == DefaultInputDelay && AutoInputDelay(InputDelayWireCeiling + 1) == DefaultInputDelay && DefaultInputDelay == 2);
    // The styles: Responsive is one frame less from 0 to 2, so a close
    // opponent plays at 0; Smooth one more from 1 to 4. With no
    // recommendation every style readies at the default.
    using S = AutoDelayStyle;
    CHECK(AutoInputDelay(1, S::Responsive) == 0 && AutoInputDelay(2, S::Responsive) == 1 && AutoInputDelay(3, S::Responsive) == 2 &&
        AutoInputDelay(5, S::Responsive) == 2);
    CHECK(AutoInputDelay(1, S::Smooth) == 2 && AutoInputDelay(3, S::Smooth) == 4 && AutoInputDelay(5, S::Smooth) == 4 &&
        AutoInputDelay(0, S::Smooth) == 1);
    for (const S style : {S::Balanced, S::Responsive, S::Smooth}) {
        CHECK(AutoInputDelay(-1, style) == DefaultInputDelay && AutoInputDelay(InputDelayWireCeiling + 1, style) == DefaultInputDelay);
        for (int recommended = 0; recommended <= InputDelayWireCeiling; ++recommended)
            CHECK(AutoInputDelay(recommended, style) >= AutoInputDelayRange(style).minimum &&
                AutoInputDelay(recommended, style) <= AutoInputDelayRange(style).maximum && AutoInputDelay(recommended, style) <= MaximumInputDelay);
    }
    CHECK(AutoInputDelay(2, S::Balanced) == AutoInputDelay(2));
    // SF4's round trip bands are the connection check's (recovery.rs, tests
    // probe_delay_uses_player_bands, probe_delay_rounds_half_milliseconds_up
    // and probe_delay_tolerates_one_worse_tail_band pin these edges in
    // microseconds): change them together. SF4 plays with them unchanged.
    const InputDelayBands sf4 = Sf4InputDelayBands;
    static_assert(Sf4InputDelayBands.firstBandMs == 80 && Sf4InputDelayBands.bandStepMs == 70, "SF4's bands");
    struct Edge { int rttMs, delay; };
    for (const Edge edge : {Edge{0, 1}, Edge{1, 1}, Edge{80, 1}, Edge{81, 2}, Edge{150, 2}, Edge{151, 3}, Edge{220, 3},
        Edge{221, 4}, Edge{290, 4}, Edge{291, 5}, Edge{360, 5}, Edge{361, 5}, Edge{640, 5}, Edge{641, 5}, Edge{710, 5},
        Edge{711, 5}, Edge{1000, 5}, Edge{1000000, 5}})
        CHECK(InputDelayBand(sf4, edge.rttMs) == edge.delay);
    for (const Edge edge : {Edge{150, 1}, Edge{151, 2}, Edge{221, 3}})
        CHECK(RecommendedInputDelay(sf4, RoundTrip{80, edge.rttMs}) == edge.delay);
    CHECK(RecommendedInputDelay(sf4, RoundTrip{151, 151}) == 3 && RecommendedInputDelay(sf4, RoundTrip{80, 80}) == RecommendedInputDelayMinimum);
    // A recommendation never passes the choice limit, however bad the line.
    CHECK(RecommendedInputDelay(sf4, RoundTrip{1000, 1000}) == MaximumInputDelay &&
        RecommendedInputDelay(sf4, RoundTrip{1000000, 1000000}) == MaximumInputDelay);
    CHECK(RecommendedInputDelay(sf4, RoundTrip{80, 1000000}) == MaximumInputDelay - 1);
    static_assert(InputDelayBand(Sf4InputDelayBands, 80) == 1 && InputDelayBand(Sf4InputDelayBands, 81) == 2 &&
        RecommendedInputDelay(Sf4InputDelayBands, RoundTrip{80, 221}) == 3, "constant expressions");
    // The check's microseconds read as the helper reads them: whole
    // milliseconds, half up, so the game's recommendation with SF4's bands is
    // the helper's own (recovery.rs: probe_delay_uses_player_bands and
    // probe_delay_rounds_half_milliseconds_up, the same rows).
    struct Micro { std::uint64_t us; int delay; };
    for (const Micro row : {Micro{0, 1}, Micro{80000, 1}, Micro{80400, 1}, Micro{80499, 1}, Micro{80500, 2}, Micro{81000, 2},
        Micro{150000, 2}, Micro{150499, 2}, Micro{150500, 3}, Micro{220499, 3}, Micro{220500, 4}, Micro{290499, 4},
        Micro{290500, 5}, Micro{361000, 5}, Micro{711000, 5}, Micro{~std::uint64_t(0), 5}})
        CHECK(RecommendedInputDelay(sf4, RoundTrip{RoundTripMs(row.us), RoundTripMs(row.us)}) == row.delay);
    CHECK(RoundTripMs(80499) == 80 && RoundTripMs(80500) == 81 && RoundTripMs(999999999) == 1000000 && RoundTripMs(~std::uint64_t(0)) == 1000000);
    // Another game's bands give its own recommendation for the same round
    // trip: here one frame through 100 ms, then one per 33 ms, so the 150 ms
    // that SF4 plays at 2 frames is 3, and a tail one band worse still counts
    // as one band, not as its milliseconds.
    const InputDelayBands other = {100, 33};
    for (const Edge edge : {Edge{100, 1}, Edge{101, 2}, Edge{133, 2}, Edge{134, 3}, Edge{150, 3}, Edge{166, 3}, Edge{167, 4},
        Edge{199, 4}, Edge{200, 5}, Edge{1000, 5}})
        CHECK(InputDelayBand(other, edge.rttMs) == edge.delay);
    CHECK(RecommendedInputDelay(sf4, RoundTrip{150, 150}) == 2 && RecommendedInputDelay(other, RoundTrip{150, 150}) == 3);
    CHECK(RecommendedInputDelay(other, RoundTrip{120, 166}) == 2 && RecommendedInputDelay(other, RoundTrip{120, 167}) == 3);
    // The delay row: Auto, then 0 to MaximumInputDelay, stopping at either end.
    CHECK(MinimumInputDelay == 0);
    CHECK(StepInputDelay(false, 2, -1) == 1 && StepInputDelay(false, 2, 1) == 3);
    CHECK(StepInputDelay(false, 1, -1) == 0 && StepInputDelay(false, 0, 1) == 1);
    CHECK(StepInputDelay(false, 0, -1) == AutoInputDelayChoice && StepInputDelay(true, 3, -1) == AutoInputDelayChoice);
    CHECK(StepInputDelay(true, 3, 1) == 0);
    CHECK(StepInputDelay(false, MaximumInputDelay, 1) == MaximumInputDelay);
    // Every step lands on Auto or a delay in range, and walking Right from
    // Auto visits Auto, 0, 1, ... MaximumInputDelay in order.
    for (int delay = MinimumInputDelay; delay <= MaximumInputDelay; ++delay)
        for (int delta : {-1, 1}) for (bool automatic : {false, true}) {
            const int next = StepInputDelay(automatic, delay, delta);
            CHECK(next == AutoInputDelayChoice || (next >= MinimumInputDelay && next <= MaximumInputDelay));
        }
    CHECK(StepInputDelay(false, MaximumInputDelay - 1, 1) == MaximumInputDelay && StepInputDelay(false, InputDelayWireCeiling, 1) == MaximumInputDelay);
    int walked = StepInputDelay(true, 5, 1);
    for (int expected = 0; expected <= MaximumInputDelay; ++expected) {
        CHECK(walked == expected);
        walked = StepInputDelay(false, walked, 1);
    }
    // The Auto choice is never a delay the room or a saved profile could hold.
    CHECK(AutoInputDelayChoice < MinimumInputDelay);
    static_assert(AutoInputDelay(-1) == 2 && StepInputDelay(true, 0, 1) == 0, "constant expressions");
    std::cout << "Auto delay bounds and delay row steps passed\n";
}
