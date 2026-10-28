// Native outcome capture, rollback correction, and confirmed-input publication.
#include <cstdio>
#include "../common/NativeMatchResult.hxx"
#include "test_support.hxx"

using namespace sf4e::native_result;

static void TestResultSurvivesContinuousRollback() {
    Timeline timeline;
    Result reported = Result::None;
    // Every outer tick replays three frames. The old observer reset on each
    // load and never reached its nine-frame age, so no result reached the room.
    for (int outer = 100; outer < 160; ++outer) {
        timeline.Rewind(outer - 3);
        for (int replay = outer - 2; replay <= outer; ++replay)
            timeline.Capture(replay, replay >= 100 ? Flow::MatchResult : Flow::Other, 0);
        const auto result = timeline.Confirmed(outer - 4);
        if (outer < 103) EXPECT(result == Result::None);
        if (result != Result::None) reported = result;
    }
    EXPECT(reported == Result::P1Win);
}

static void TestPredictionCannotAwardWinner() {
    Timeline timeline;
    timeline.Capture(100, Flow::MatchResult, 0);
    EXPECT(timeline.Confirmed(-1) == Result::None);
    EXPECT(timeline.Confirmed(98) == Result::None);
    EXPECT(timeline.Confirmed(99) == Result::P1Win); // state N includes input N-1
    timeline.Rewind(98);
    EXPECT(timeline.Confirmed(110) == Result::None); // abandoned predicted win
    timeline.Capture(99, Flow::Other, -1);
    timeline.Capture(100, Flow::MatchResult, 1);
    EXPECT(timeline.Confirmed(98) == Result::None);
    EXPECT(timeline.Confirmed(99) == Result::P2Win);
    timeline.Capture(100, Flow::Other, -1); // corrected same-frame recapture
    EXPECT(timeline.Confirmed(110) == Result::None);
}

static void TestRetainedTimelineAndShortResultFlow() {
    Timeline timeline;
    timeline.Capture(100, Flow::MatchResult, 1);
    timeline.Capture(101, Flow::Other, -1); // native flow moves on before input acknowledgement
    timeline.Rewind(100);
    EXPECT(timeline.Confirmed(99) == Result::P2Win);
    timeline.Capture(101, Flow::Other, -1);
    EXPECT(timeline.Confirmed(100) == Result::P2Win);
    timeline.Reset(); // next match must not inherit the previous winner
    EXPECT(timeline.Confirmed(1000) == Result::None);
}

static void TestDrawInvalidFramesAndLongMatches() {
    Timeline timeline;
    for (int winner : {-1, 2}) timeline.Capture(100, Flow::MatchResult, winner);
    timeline.Capture(0, Flow::MatchResult, 0);
    timeline.Capture(-1, Flow::MatchResult, 0);
    EXPECT(timeline.Confirmed(100) == Result::None);
    timeline.Capture(32768, Flow::DrawResult, -1);
    EXPECT(timeline.Confirmed(32766) == Result::None);
    EXPECT(timeline.Confirmed(32767) == Result::Draw);
    timeline.Rewind(-1); // load without a GGPO identity
    EXPECT(timeline.Confirmed(40000) == Result::None);
    for (int frame = 40000; frame < 40200; ++frame) {
        timeline.Capture(frame, Flow::MatchResult, 0);
        if (frame > 40000) EXPECT(timeline.Confirmed(frame - 2) == Result::P1Win);
    }
    timeline.Reset();
    timeline.Capture(100, Flow::MatchResult, 0);
    timeline.Capture(100 + Timeline::Capacity, Flow::Other, -1);
    EXPECT(timeline.Confirmed(1000) == Result::None); // overwritten ring slot
}

// Simulation stops at the end of the fight while the peer's inputs for the
// result frames are still in flight. No further capture happens, so the
// outcome must still be publishable when confirmation arrives later from a
// plain GGPO poll.
static void TestConfirmationAfterSimulationStops() {
    Timeline timeline;
    for (int frame = 500; frame < 540; ++frame)
        timeline.Capture(frame, frame >= 520 ? Flow::MatchResult : Flow::Other, 1);
    EXPECT(timeline.Confirmed(515) == Result::None);
    EXPECT(timeline.Latest().frame == 539 && timeline.Latest().result == Result::P2Win);
    EXPECT(timeline.Confirmed(519) == Result::P2Win);
    EXPECT(timeline.Confirmed(600) == Result::P2Win);
    timeline.Reset();
    EXPECT(timeline.Latest().frame == -1 && timeline.Latest().result == Result::None);
}

// After a result, the battle may go on to a game in place: only a confirmed
// fight after the result's frame says so, never a predicted one.
static void TestFightAfterResult() {
    Timeline timeline;
    timeline.Capture(100, Flow::Fight, -1);
    timeline.Capture(110, Flow::MatchResult, 1);
    timeline.Capture(120, Flow::Other, -1);
    CHECK(timeline.ConfirmedFightAfter(110, 200) == -1);
    timeline.Capture(125, Flow::Fight, -1);
    timeline.Capture(126, Flow::Fight, -1);
    CHECK(timeline.ConfirmedFightAfter(110, 120) == -1);
    CHECK(timeline.ConfirmedFightAfter(110, 130) == 125);
    timeline.Rewind(121);
    CHECK(timeline.ConfirmedFightAfter(110, 130) == -1);
}

int main() {
    TestResultSurvivesContinuousRollback();
    TestConfirmationAfterSimulationStops();
    TestPredictionCannotAwardWinner();
    TestRetainedTimelineAndShortResultFlow();
    TestDrawInvalidFramesAndLongMatches();
    TestFightAfterResult();
    if (TEST_FAILURES()) std::printf("%d native result test(s) failed\n", TEST_FAILURES());
    else std::puts("Native result confirmation and continuous-rollback tests passed");
    return TEST_EXIT_CODE();
}
