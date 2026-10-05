#include "../netplay/AutoDelayCheck.hxx"
#include <iostream>
#include "test_support.hxx"
int main() {
    using sf4e::netplay::AutoDelayCheck;
    const std::uint64_t settle = AutoDelayCheck::SettleMs, giveUp = AutoDelayCheck::GiveUpMs;
    AutoDelayCheck check;
    // An empty seat wants nothing and Auto readies with two frames.
    CHECK(!check.Wanted(0) && !check.Due(0) && !check.Measured("") && check.Delay("") == 2);

    // An opponent sits: the check is wanted at once and due after the table settles.
    check.Seat("a", 5, 1000);
    CHECK(check.Opponent() == "a" && check.Wanted(1000) && !check.Due(1000 + settle - 1) && check.Due(1000 + settle));
    // The same opponent seen again does not restart the wait.
    check.Seat("a", 9, 1500);
    CHECK(check.Due(1000 + settle));
    // A check the room never takes stops being wanted.
    CHECK(check.Wanted(1000 + settle + giveUp - 1) && !check.Wanted(1000 + settle + giveUp));

    // Once asked for, it is not asked for again, measured or not.
    check.Asked();
    CHECK(!check.Wanted(1000 + settle) && !check.Measured("a") && check.Delay("a") == 2);
    // Results for another peer, from before the opponent sat, or without a recommendation do not count.
    check.Observe("b", 5, 1);
    check.Observe("a", 4, 1);
    check.Observe("a", 5, -1);
    CHECK(!check.Measured("a"));
    // A result against this opponent does, held between Auto's bounds.
    check.Observe("a", 5, 0);
    CHECK(check.Measured("a") && check.Delay("a") == 1 && !check.Measured("b") && check.Delay("b") == 2);
    check.Observe("a", 6, 7);
    CHECK(check.Delay("a") == 3);
    // Choosing Auto again leaves a measured opponent alone.
    check.Want(9000);
    CHECK(!check.Wanted(9000 + settle));

    // A new opponent starts over; a check made by hand counts as well.
    check.Seat("b", 7, 20000);
    CHECK(!check.Measured("b") && check.Delay("b") == 2 && check.Due(20000 + settle));
    check.Observe("b", 7, 2);
    CHECK(check.Measured("b") && check.Delay("b") == 2 && !check.Wanted(20000 + settle));
    // An opponent who leaves and returns is measured again.
    check.Seat("", 8, 30000);
    CHECK(!check.Wanted(30000 + settle) && !check.Measured(""));
    check.Seat("b", 8, 31000);
    check.Observe("b", 7, 2);
    CHECK(!check.Measured("b") && check.Due(31000 + settle));

    // Auto chosen after the first wait ran out wants the check again.
    check.Want(60000);
    CHECK(!check.Due(60000) && check.Due(60000 + settle));
    std::cout << "Auto delay check passed\n";
}
