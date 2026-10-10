#include "../netplay/AutoDelayCheck.hxx"
#include <iostream>
#include "test_support.hxx"
using sf4e::netplay::AutoDelayCheck;
static const std::uint64_t settle = AutoDelayCheck::SettleMs, giveUp = AutoDelayCheck::GiveUpMs,
    retry = AutoDelayCheck::RetryMs, hold = AutoDelayCheck::HoldMs, returnMs = AutoDelayCheck::ReturnMs;

// The check's schedule and which results count.
static void Schedule() {
    AutoDelayCheck check;
    // An empty seat wants nothing and Auto readies with two frames.
    CHECK(!check.Wanted(0) && !check.Due(0) && !check.Holding(0) && !check.Measured("") && check.Delay("") == 2);

    // An opponent sits: the check is wanted at once and due after the table settles.
    check.Seat("a", 40, 5, 1000);
    CHECK(check.Wanted(1000) && check.Holding(1000) && !check.Due(1000 + settle - 1) && check.Due(1000 + settle));
    // The same opponent seen again does not restart the wait.
    check.Seat("a", 40, 9, 1500);
    CHECK(check.Due(1000 + settle));
    // A check the room never takes stops being wanted, and Ready stops holding.
    CHECK(check.Wanted(1000 + settle + giveUp - 1) && !check.Wanted(1000 + settle + giveUp) &&
        !check.Holding(1000 + settle + giveUp));

    // Once asked for, it is not asked for again while it runs, and Ready holds for it.
    check.Asked(5);
    check.Observe("a", 5, "checking", -1, 1000 + settle);
    CHECK(!check.Wanted(1000 + settle) && check.Holding(1000 + settle) && !check.Measured("a") && check.Delay("a") == 2);
    // Results for another peer, from before the opponent sat, or without a recommendation do not count.
    check.Observe("b", 5, "complete", 1, 1000 + settle);
    check.Observe("a", 4, "complete", 1, 1000 + settle);
    check.Observe("a", 5, "complete", -1, 1000 + settle);
    CHECK(!check.Measured("a"));
    // A result against this opponent does, held between Auto's bounds.
    check.Observe("a", 5, "complete", 0, 3000);
    CHECK(check.Measured("a") && check.Delay("a") == 1 && !check.Measured("b") && check.Delay("b") == 2 && !check.Holding(3000));
    check.Observe("a", 6, "complete", 7, 3000);
    CHECK(check.Delay("a") == 4);
    // Choosing Auto again leaves a measured opponent alone.
    check.Want(9000);
    CHECK(!check.Wanted(9000 + settle) && !check.Holding(9000));

    // A new opponent starts over; a check made by hand counts as well.
    check.Seat("b", 50, 7, 20000);
    CHECK(!check.Measured("b") && check.Delay("b") == 2 && check.Due(20000 + settle));
    check.Observe("b", 7, "complete", 2, 20100);
    CHECK(check.Measured("b") && check.Delay("b") == 2 && !check.Wanted(20000 + settle));

    // Auto chosen after the first wait ran out wants the check again.
    check.Seat("c", 60, 8, 40000);
    CHECK(!check.Wanted(40000 + settle + giveUp));
    check.Want(60000);
    CHECK(!check.Due(60000) && check.Due(60000 + settle) && check.Holding(60000));
}

// Both fighters on Auto ask at the same moment and the room commits one
// reservation at a time, so a check that ends without a recommendation, or
// never starts, is asked for once more, and only once.
static void RetryOnce() {
    AutoDelayCheck check;
    check.Seat("a", 10, 1, 0);
    check.Asked(1);
    check.Observe("a", 1, "checking", -1, settle);
    check.Observe("a", 1, "unavailable", -1, settle + 200);
    CHECK(check.Holding(settle + 200) && !check.Due(settle + 200 + retry - 1) && check.Due(settle + 200 + retry));
    // The second attempt never starts: the room's check still shows the first request.
    check.Asked(2);
    check.Observe("a", 1, "unavailable", -1, settle + 200 + retry);
    CHECK(!check.Wanted(settle + 200 + retry * 2) && !check.Holding(settle + 200 + retry) && check.Delay("a") == 2);

    // A retry that measures counts as any check does.
    AutoDelayCheck again;
    again.Seat("a", 10, 1, 0);
    again.Asked(1);
    again.Observe("a", 1, "unavailable", -1, settle + 100);
    again.Asked(2);
    again.Observe("a", 2, "checking", -1, settle + 100 + retry);
    again.Observe("a", 2, "complete", 3, settle + 100 + retry + 5000);
    CHECK(again.Measured("a") && again.Delay("a") == 3 && !again.Holding(settle + 100 + retry + 5000));
    // A host handoff mid-check: the new term clears the room's check (no
    // request, no status), and Auto asks once more while the hold lasts.
    AutoDelayCheck handoff;
    handoff.Seat("a", 10, 1, 0);
    handoff.Asked(1);
    handoff.Observe("a", 1, "checking", -1, settle);
    handoff.Observe("", 0, "", -1, 3000);
    CHECK(handoff.Holding(3000) && !handoff.Due(3000 + retry - 1) && handoff.Due(3000 + retry));
    // The check named a revision the room had already moved past (a spectator
    // pressed Watch as the seats filled, and this view had not caught up), so
    // the helper refused it. The retry waits for the view to move and settle.
    AutoDelayCheck stale;
    stale.Seat("a", 3, 1, 0);
    stale.Asked(1);
    stale.Observe("a", 1, "unavailable", -1, settle + 100);
    stale.Seat("a", 4, 1, settle + 800);
    CHECK(!stale.Due(settle + 100 + retry) && stale.Due(settle + 800 + settle) && stale.Holding(settle + 800 + settle));
}

// Ready never waits on the check past HoldMs: a check that never answers, a
// slow relay, or a tournament start all go ahead with two frames.
static void HoldIsBounded() {
    AutoDelayCheck check;
    check.Seat("a", 10, 1, 0);
    check.Asked(1);
    check.Observe("a", 1, "checking", -1, settle);
    CHECK(check.Holding(hold - 1) && !check.Holding(hold) && check.Delay("a") == 2);
    // The check ending after the hold still measures the opponent for later games.
    check.Observe("a", 1, "complete", 3, hold + 2000);
    CHECK(check.Measured("a") && check.Delay("a") == 3);
    // A check that times out after the hold is not asked for again.
    AutoDelayCheck late;
    late.Seat("a", 10, 1, 0);
    late.Asked(1);
    late.Observe("a", 1, "timed_out", -1, 26000);
    CHECK(!late.Wanted(26000 + retry) && !late.Holding(26000 + retry));
    // Choosing Auto again while a check runs leaves it running.
    AutoDelayCheck running;
    running.Seat("a", 10, 1, 0);
    running.Asked(1);
    running.Want(500);
    running.Observe("a", 1, "checking", -1, 600);
    CHECK(!running.Wanted(600) && running.Holding(600));
}

// The table's revision moves when anyone readies, queues or watches there. A
// check waits for it to settle, since the opponent's helper refuses a pair it
// has not caught up with, but a measured opponent stays measured.
static void RevisionMoves() {
    AutoDelayCheck check;
    check.Seat("a", 10, 1, 0);
    // A spectator starts watching just before the check is due.
    check.Seat("a", 11, 1, settle - 100);
    CHECK(!check.Due(settle) && check.Due(settle - 100 + settle) && check.Holding(settle));
    check.Asked(1);
    check.Observe("a", 1, "complete", 2, 6000);
    // The opponent changes fighter, which withdraws Ready and moves the
    // revision: the network did not change, so nothing is checked again.
    check.Seat("a", 12, 2, 7000);
    CHECK(check.Measured("a") && !check.Wanted(7000 + settle) && !check.Holding(7000 + settle) && check.Delay("a") == 2);
}

// Queue rotation: a new opponent between sets is checked, a rematch is not.
// An opponent who is briefly gone (a room recovering, a host handoff) keeps
// the measurement; one gone for longer is measured again.
static void Opponents() {
    AutoDelayCheck check;
    check.Seat("a", 10, 1, 0);
    check.Observe("a", 1, "complete", 3, 6000);
    // Rematch: same opponent, later revisions.
    check.Seat("a", 15, 2, 60000);
    CHECK(check.Measured("a") && !check.Holding(60000));
    // The set ends and the queue seats someone else.
    check.Seat("b", 20, 3, 120000);
    CHECK(!check.Measured("b") && check.Holding(120000) && check.Due(120000 + settle) && check.Delay("b") == 2);
    check.Observe("b", 3, "complete", 1, 126000);
    CHECK(check.Measured("b") && check.Delay("b") == 1);
    // The room recovers: no opponent for a moment, then the same one.
    check.Seat("", 0, 4, 130000);
    CHECK(!check.Holding(130000) && !check.Wanted(130000));
    check.Seat("b", 21, 4, 133000);
    CHECK(check.Measured("b") && !check.Holding(133000) && check.Delay("b") == 1);
    // Gone for longer than ReturnMs: measured again.
    check.Seat("", 0, 5, 140000);
    check.Seat("b", 30, 5, 140000 + returnMs);
    CHECK(!check.Measured("b") && check.Due(140000 + returnMs + settle));
    // Back to the first opponent: their old measurement no longer counts.
    check.Seat("a", 31, 5, 200000);
    CHECK(!check.Measured("a") && check.Due(200000 + settle));
    check.Observe("a", 1, "complete", 3, 200000 + settle);
    CHECK(!check.Measured("a"));
    // A check under way when the room flickers still counts once it ends.
    AutoDelayCheck flicker;
    flicker.Seat("a", 10, 1, 0);
    flicker.Asked(1);
    flicker.Observe("a", 1, "checking", -1, settle);
    flicker.Seat("", 0, 2, 2000);
    flicker.Seat("a", 11, 2, 2500);
    CHECK(flicker.Holding(2500) && !flicker.Wanted(2500));
    flicker.Observe("a", 1, "complete", 2, 6000);
    CHECK(flicker.Measured("a") && flicker.Delay("a") == 2 && !flicker.Holding(6000));
}

// A spectator or a queued member has no opponent: nothing is wanted, nothing
// holds, and the delay is never measured for them.
static void Spectators() {
    AutoDelayCheck check;
    for (std::uint64_t now = 0; now < 30000; now += 500) {
        check.Seat("", 0, 1, now);
        check.Observe("a", 1, "checking", -1, now);
        CHECK(!check.Wanted(now) && !check.Due(now) && !check.Holding(now));
    }
    check.Want(30000);
    CHECK(!check.Wanted(30000 + settle) && !check.Holding(30000 + settle));
}

int main() {
    Schedule();
    RetryOnce();
    HoldIsBounded();
    RevisionMoves();
    Opponents();
    Spectators();
    std::cout << "Auto delay check passed\n";
}
