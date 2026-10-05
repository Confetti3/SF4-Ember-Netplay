#include "../common/InputDelay.hxx"
#include <cstdlib>
#include <iostream>
#include "test_support.hxx"
int main() {
    using namespace sf4e;
    // Auto holds the recommendation between its bounds.
    CHECK(AutoInputDelay(0) == 1 && AutoInputDelay(1) == 1 && AutoInputDelay(2) == 2 && AutoInputDelay(3) == 3);
    CHECK(AutoInputDelay(4) == 3 && AutoInputDelay(MaximumInputDelay) == 3);
    for (int recommended = 0; recommended <= MaximumInputDelay; ++recommended)
        CHECK(AutoInputDelay(recommended) >= AutoInputDelayMinimum && AutoInputDelay(recommended) <= AutoInputDelayMaximum);
    // No recommendation, or one outside the delay range, is a new profile's two frames.
    CHECK(AutoInputDelay(-1) == 2 && AutoInputDelay(MaximumInputDelay + 1) == 2);
    // The delay row: Auto, then 0 to MaximumInputDelay, stopping at either end.
    CHECK(StepInputDelay(false, 2, -1) == 1 && StepInputDelay(false, 1, -1) == 0 && StepInputDelay(false, 2, 1) == 3);
    CHECK(StepInputDelay(false, 0, -1) == AutoInputDelayChoice && StepInputDelay(true, 3, -1) == AutoInputDelayChoice);
    CHECK(StepInputDelay(true, 3, 1) == 0);
    CHECK(StepInputDelay(false, MaximumInputDelay, 1) == MaximumInputDelay);
    std::cout << "Auto delay bounds and delay row steps passed\n";
}
