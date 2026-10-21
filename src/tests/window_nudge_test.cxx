// The game window's nudge: due at once, then about once a second however
// often the frame asks.
#include "../common/WindowNudge.hxx"
#include "test_support.hxx"

using sf4e::window_nudge::IntervalMs;
using sf4e::window_nudge::Nudge;

int main() {
    Nudge nudge;
    CHECK(nudge.Due(5000));
    // Every frame of the next second asks; none is due.
    for (std::uint64_t at = 5000; at < 5000 + IntervalMs; at += 16) CHECK(!nudge.Due(at));
    CHECK(nudge.Due(5000 + IntervalMs));
    // A late frame is not made up for: the next one counts from it.
    CHECK(nudge.Due(5000 + 3 * IntervalMs + 7));
    CHECK(!nudge.Due(5000 + 4 * IntervalMs));
    CHECK(nudge.Due(5000 + 4 * IntervalMs + 7));
    // The first call is due whatever the clock reads.
    Nudge fresh;
    CHECK(fresh.Due(0));
    CHECK(!fresh.Due(IntervalMs - 1));
    std::puts("Window nudge timing passed");
    return 0;
}
