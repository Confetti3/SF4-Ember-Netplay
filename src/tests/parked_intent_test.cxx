#include "../netplay/ParkedIntent.hxx"

#include <cstdio>

using namespace sf4e::netplay;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

using Intent = ParkedIntent<int>;
static const Generation Current{1, 4};

// Room action: attempt, defer, retry, defer again, then dispatched.
static void TestRetryKeepsItsBudgetUntilDispatched() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(7, Current, 0, Intent::Budget::Timed);
    auto attempt = intent.Take();
    CHECK(attempt && *attempt == 7 && !intent.Parked() && intent.Armed());
    intent.Defer(*attempt, Current, 2000, Intent::Budget::Timed);
    intent.Settle(DispatchOutcome::Deferred);
    CHECK(intent.Parked() && !intent.Expired(2999) && intent.Expired(3000));
    attempt = intent.Take();
    intent.Settle(DispatchOutcome::Dispatched);
    CHECK(!intent.Active());
}

static void TestDroppedRetryEndsARoomAction() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(1, Current, 0, Intent::Budget::Timed);
    intent.Take();
    intent.Settle(DispatchOutcome::Dropped);
    CHECK(!intent.Active());
}

// A newer press supersedes a parked one whether or not it parks itself.
static void TestNewerPressSupersedes() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(1, Current, 0, Intent::Budget::Timed);
    intent.Clear();
    CHECK(!intent.Active());
    intent.Defer(2, Current, 2500, Intent::Budget::Timed);
    CHECK(*intent.Parked() == 2 && !intent.Expired(3000) && intent.Expired(5500));
}

static void TestUntimedDeferralWaitsUntilATimedOne() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(3, Current, 0, Intent::Budget::Untimed);
    CHECK(intent.Active() && !intent.Armed() && !intent.Expired(1000000));
    // The teardown ended and the fence refused the retry: the budget starts now.
    auto attempt = intent.Take();
    intent.Defer(*attempt, Current, 1000000, Intent::Budget::Timed);
    intent.Settle(DispatchOutcome::Deferred);
    CHECK(!intent.Expired(1002999) && intent.Expired(1003000));
    // An untimed deferral of a timed intent keeps its budget.
    attempt = intent.Take();
    intent.Defer(*attempt, Current, 1001000, Intent::Budget::Untimed);
    CHECK(intent.Expired(1003000));
}

static void TestPauseRestartsTheFullBudget() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(4, Current, 0, Intent::Budget::Timed);
    CHECK(!intent.Expired(2900, true));
    CHECK(!intent.Expired(5899));
    CHECK(intent.Expired(5900));
}

// Ready: armed at the press, parked behind the drain, sent, then committed.
static void TestReadyAwaitsItsCommit() {
    Intent intent(20000, Intent::Completion::OnCommit);
    intent.Arm(100, Current);
    intent.Defer(5, Current, 200, Intent::Budget::Timed);
    intent.Commit();
    CHECK(intent.Parked());
    intent.Take();
    intent.Settle(DispatchOutcome::Dispatched);
    CHECK(intent.AwaitingCommit() && !intent.Expired(20099) && intent.Expired(20100));
    intent.Commit();
    CHECK(!intent.Active());
}

static void TestWithdrawnReadyStillReportsAStall() {
    Intent intent(20000, Intent::Completion::OnCommit);
    intent.Arm(0, Current);
    intent.Defer(6, Current, 0, Intent::Budget::Timed);
    intent.Withdraw();
    CHECK(!intent.Parked() && intent.Armed() && intent.Expired(20000));
}

static void TestStaleGenerationEndsTheIntent() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(6, Current, 0, Intent::Budget::Timed);
    intent.DropStale(Current);
    CHECK(intent.Parked() && intent.Armed());
    intent.DropStale(Generation{1, 5});
    CHECK(!intent.Active());
    intent.Arm(0, Current);
    intent.DropStale(Generation{2, 0});
    CHECK(!intent.Active());
}

int main() {
    TestRetryKeepsItsBudgetUntilDispatched();
    TestDroppedRetryEndsARoomAction();
    TestNewerPressSupersedes();
    TestUntimedDeferralWaitsUntilATimedOne();
    TestPauseRestartsTheFullBudget();
    TestReadyAwaitsItsCommit();
    TestWithdrawnReadyStillReportsAStall();
    TestStaleGenerationEndsTheIntent();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ParkedIntent tests passed\n");
    return failures ? 1 : 0;
}
