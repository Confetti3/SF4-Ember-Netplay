#include "../netplay/ParkedIntent.hxx"

#include <cstdio>
#include "test_support.hxx"

using namespace sf4e::netplay;

using Intent = ParkedIntent<int>;
static const Generation Current{1, 4};

// Room action: attempt, defer, retry, defer again, then dispatched.
static void TestRetryKeepsItsBudgetUntilDispatched() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(7, Current, 0, Intent::Budget::Timed);
    auto attempt = intent.Take();
    EXPECT(attempt && *attempt == 7 && !intent.Parked() && intent.Armed());
    intent.Defer(*attempt, Current, 2000, Intent::Budget::Timed);
    intent.Settle(DispatchOutcome::Deferred);
    EXPECT(intent.Parked() && !intent.Expired(2999) && intent.Expired(3000));
    attempt = intent.Take();
    intent.Settle(DispatchOutcome::Dispatched);
    EXPECT(!intent.Active());
}

static void TestDroppedRetryEndsARoomAction() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(1, Current, 0, Intent::Budget::Timed);
    intent.Take();
    intent.Settle(DispatchOutcome::Dropped);
    EXPECT(!intent.Active());
}

// A newer press supersedes a parked one whether or not it parks itself.
static void TestNewerPressSupersedes() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(1, Current, 0, Intent::Budget::Timed);
    intent.Clear();
    EXPECT(!intent.Active());
    intent.Defer(2, Current, 2500, Intent::Budget::Timed);
    EXPECT(*intent.Parked() == 2 && !intent.Expired(3000) && intent.Expired(5500));
}

static void TestUntimedDeferralWaitsUntilATimedOne() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(3, Current, 0, Intent::Budget::Untimed);
    EXPECT(intent.Active() && !intent.Armed() && !intent.Expired(1000000));
    // The teardown ended and the fence refused the retry: the budget starts now.
    auto attempt = intent.Take();
    intent.Defer(*attempt, Current, 1000000, Intent::Budget::Timed);
    intent.Settle(DispatchOutcome::Deferred);
    EXPECT(!intent.Expired(1002999) && intent.Expired(1003000));
    // An untimed deferral of a timed intent keeps its budget.
    attempt = intent.Take();
    intent.Defer(*attempt, Current, 1001000, Intent::Budget::Untimed);
    EXPECT(intent.Expired(1003000));
}

static void TestPauseRestartsTheFullBudget() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(4, Current, 0, Intent::Budget::Timed);
    EXPECT(!intent.Expired(2900, true));
    EXPECT(!intent.Expired(5899));
    EXPECT(intent.Expired(5900));
}

// Ready: armed at the press, parked behind the drain, sent, then committed.
static void TestReadyAwaitsItsCommit() {
    Intent intent(20000, Intent::Completion::OnCommit);
    intent.Arm(100, Current);
    intent.Defer(5, Current, 200, Intent::Budget::Timed);
    intent.Commit();
    EXPECT(intent.Parked());
    intent.Take();
    intent.Settle(DispatchOutcome::Dispatched);
    EXPECT(intent.AwaitingCommit() && !intent.Expired(20099) && intent.Expired(20100));
    intent.Commit();
    EXPECT(!intent.Active());
}

static void TestWithdrawnReadyStillReportsAStall() {
    Intent intent(20000, Intent::Completion::OnCommit);
    intent.Arm(0, Current);
    intent.Defer(6, Current, 0, Intent::Budget::Timed);
    intent.Withdraw();
    EXPECT(!intent.Parked() && intent.Armed() && intent.Expired(20000));
}

static void TestStaleGenerationEndsTheIntent() {
    Intent intent(3000, Intent::Completion::OnDispatch);
    intent.Defer(6, Current, 0, Intent::Budget::Timed);
    intent.DropStale(Current);
    EXPECT(intent.Parked() && intent.Armed());
    intent.DropStale(Generation{1, 5});
    EXPECT(!intent.Active());
    intent.Arm(0, Current);
    intent.DropStale(Generation{2, 0});
    EXPECT(!intent.Active());
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
    if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
    else std::printf("ParkedIntent tests passed\n");
    return TEST_EXIT_CODE();
}
