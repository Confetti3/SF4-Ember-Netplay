#include "../common/InputDelay.hxx"
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
    // The round trip bands are the connection check's (recovery.rs, tests
    // probe_delay_uses_player_bands and probe_delay_tolerates_one_worse_tail_band
    // pin these edges in microseconds): change them together.
    struct Edge { int rttMs, delay; };
    for (const Edge edge : {Edge{0, 1}, Edge{1, 1}, Edge{80, 1}, Edge{81, 2}, Edge{150, 2}, Edge{151, 3}, Edge{220, 3},
        Edge{221, 4}, Edge{290, 4}, Edge{291, 5}, Edge{360, 5}, Edge{361, 5}, Edge{640, 5}, Edge{641, 5}, Edge{710, 5},
        Edge{711, 5}, Edge{1000, 5}, Edge{1000000, 5}})
        CHECK(InputDelayBand(edge.rttMs) == edge.delay);
    for (const Edge edge : {Edge{150, 1}, Edge{151, 2}, Edge{221, 3}})
        CHECK(RecommendedInputDelay(80, edge.rttMs) == edge.delay);
    CHECK(RecommendedInputDelay(151, 151) == 3 && RecommendedInputDelay(80, 80) == RecommendedInputDelayMinimum);
    // A recommendation never passes the choice limit, however bad the line.
    CHECK(RecommendedInputDelay(1000, 1000) == MaximumInputDelay && RecommendedInputDelay(1000000, 1000000) == MaximumInputDelay);
    CHECK(RecommendedInputDelay(80, 1000000) == MaximumInputDelay - 1);
    static_assert(InputDelayBand(80) == 1 && InputDelayBand(81) == 2 && RecommendedInputDelay(80, 221) == 3, "constant expressions");
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
