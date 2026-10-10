#include "../common/InputDelay.hxx"
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include "test_support.hxx"
int main() {
    using namespace sf4e;
    // Auto holds the recommendation between its bounds.
    CHECK(AutoInputDelay(0) == 1 && AutoInputDelay(1) == 1 && AutoInputDelay(2) == 2 && AutoInputDelay(3) == 3);
    CHECK(AutoInputDelay(4) == 3 && AutoInputDelay(MaximumInputDelay) == 3);
    for (int recommended = 0; recommended <= MaximumInputDelay; ++recommended)
        CHECK(AutoInputDelay(recommended) >= AutoInputDelayMinimum && AutoInputDelay(recommended) <= AutoInputDelayMaximum);
    // No recommendation, or one outside the delay range, uses Auto's two-frame fallback.
    CHECK(AutoInputDelay(-1) == 2 && AutoInputDelay(MaximumInputDelay + 1) == 2);
    // The delay row: Auto, then MinimumInputDelay to MaximumInputDelay, stopping at either end.
    CHECK(StepInputDelay(false, 2, -1) == 1 && StepInputDelay(false, 2, 1) == 3);
    CHECK(StepInputDelay(false, MinimumInputDelay, -1) == AutoInputDelayChoice && StepInputDelay(true, 3, -1) == AutoInputDelayChoice);
    CHECK(StepInputDelay(true, 3, 1) == MinimumInputDelay && MinimumInputDelay == 0);
    CHECK(StepInputDelay(false, MaximumInputDelay, 1) == MaximumInputDelay);
    // No step goes below zero except as Auto.
    for (int delay = 0; delay <= MaximumInputDelay; ++delay)
        for (int delta : {-1, 1}) for (bool automatic : {false, true}) {
            const int next = StepInputDelay(automatic, delay, delta);
            CHECK(next == AutoInputDelayChoice || (next >= MinimumInputDelay && next <= MaximumInputDelay));
        }
    // Zero steps Right to 1 and Left to Auto.
    CHECK(StepInputDelay(false, 0, 1) == 1 && StepInputDelay(false, 0, -1) == AutoInputDelayChoice);
    // The Auto choice is never a delay the room or a saved profile could hold.
    CHECK(AutoInputDelayChoice < 0 && SavedInputDelay(AutoInputDelayChoice) == 1);
    CHECK(DefaultInputDelay == 1);
    // Zero survives play and persistence; invalid saved values use one frame.
    CHECK(PlayableInputDelay(0) == MinimumInputDelay && PlayableInputDelay(3) == 3);
    CHECK(SavedInputDelay(0) == 0 && SavedInputDelay(4) == 4 && SavedInputDelay(-1) == 1 && SavedInputDelay(MaximumInputDelay + 1) == 1);
    static_assert(AutoInputDelay(-1) == 2 && PlayableInputDelay(0) == 0, "constant expressions");
    std::cout << "Auto delay bounds and delay row steps passed\n";
}
