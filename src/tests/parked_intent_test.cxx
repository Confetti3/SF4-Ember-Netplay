#include "../netplay/ParkedIntent.hxx"

#include <cstdio>

using namespace sf4e::netplay;
static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; } } while (false)

static const Generation Current{1, 4};

static void TestRetryKeepsItsBudget() {
    ParkedIntent<int> intent(3000);
    intent.Replace(7, Current); intent.Arm(0, Current);
    auto attempt = intent.Take();
    CHECK(attempt && *attempt == 7 && !intent.Parked() && intent.Armed());
    intent.Park(*attempt, Current); intent.Arm(2000, Current);
    CHECK(!intent.Expired(2999));
    CHECK(intent.Expired(3000));
}

static void TestFreshPressStartsItsOwnBudget() {
    ParkedIntent<int> intent(3000);
    intent.Replace(1, Current); intent.Arm(0, Current);
    intent.Replace(2, Current); intent.Arm(2500, Current);
    CHECK(*intent.Parked() == 2);
    CHECK(!intent.Expired(3000));
    CHECK(intent.Expired(5500));
}

static void TestHeldIntentNeverExpiresUntilArmed() {
    ParkedIntent<int> intent(3000);
    intent.Replace(3, Current);
    CHECK(intent.Active() && !intent.Armed() && !intent.Expired(1000000));
    // A held action that is refused again by the fence starts its budget then.
    intent.Arm(1000000, Current);
    CHECK(!intent.Expired(1002999) && intent.Expired(1003000));
}

static void TestPauseRestartsTheFullBudget() {
    ParkedIntent<int> intent(3000);
    intent.Replace(4, Current); intent.Arm(0, Current);
    CHECK(!intent.Expired(2900, true));
    CHECK(!intent.Expired(5899));
    CHECK(intent.Expired(5900));
}

static void TestBudgetOutlivesTheSentCommand() {
    ParkedIntent<int> intent(20000);
    intent.Arm(100, Current);
    CHECK(intent.Active() && intent.Armed() && !intent.Parked());
    intent.Park(5, Current);
    intent.Take();
    CHECK(intent.Active() && !intent.Expired(20099) && intent.Expired(20100));
    intent.Clear();
    CHECK(!intent.Active() && !intent.Expired(100000));
}

static void TestStaleGenerationEndsTheIntent() {
    ParkedIntent<int> intent(3000);
    intent.Replace(6, Current); intent.Arm(0, Current);
    intent.DropStale(Current);
    CHECK(intent.Parked() && intent.Armed());
    intent.DropStale(Generation{1, 5});
    CHECK(!intent.Active());
    intent.Arm(0, Current);
    intent.DropStale(Generation{2, 0});
    CHECK(!intent.Active());
}

int main() {
    TestRetryKeepsItsBudget();
    TestFreshPressStartsItsOwnBudget();
    TestHeldIntentNeverExpiresUntilArmed();
    TestPauseRestartsTheFullBudget();
    TestBudgetOutlivesTheSentCommand();
    TestStaleGenerationEndsTheIntent();
    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("ParkedIntent tests passed\n");
    return failures ? 1 : 0;
}
