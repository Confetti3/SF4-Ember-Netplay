// Native outcome capture, rollback correction, and confirmed-input publication.
#include <cstdio>
#include "../common/NativeMatchResult.hxx"

using namespace sf4e::native_result;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (false)

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
        if (outer < 103) CHECK(result == Result::None);
        if (result != Result::None) reported = result;
    }
    CHECK(reported == Result::P1Win);
}

static void TestPredictionCannotAwardWinner() {
    Timeline timeline;
    timeline.Capture(100, Flow::MatchResult, 0);
    CHECK(timeline.Confirmed(-1) == Result::None);
    CHECK(timeline.Confirmed(98) == Result::None);
    CHECK(timeline.Confirmed(99) == Result::P1Win); // state N includes input N-1
    timeline.Rewind(98);
    CHECK(timeline.Confirmed(110) == Result::None); // abandoned predicted win
    timeline.Capture(99, Flow::Other, -1);
    timeline.Capture(100, Flow::MatchResult, 1);
    CHECK(timeline.Confirmed(98) == Result::None);
    CHECK(timeline.Confirmed(99) == Result::P2Win);
    timeline.Capture(100, Flow::Other, -1); // corrected same-frame recapture
    CHECK(timeline.Confirmed(110) == Result::None);
}

static void TestRetainedTimelineAndShortResultFlow() {
    Timeline timeline;
    timeline.Capture(100, Flow::MatchResult, 1);
    timeline.Capture(101, Flow::Other, -1); // native flow moves on before input acknowledgement
    timeline.Rewind(100);
    CHECK(timeline.Confirmed(99) == Result::P2Win);
    timeline.Capture(101, Flow::Other, -1);
    CHECK(timeline.Confirmed(100) == Result::P2Win);
    timeline.Reset(); // next match must not inherit the previous winner
    CHECK(timeline.Confirmed(1000) == Result::None);
}

static void TestDrawInvalidFramesAndLongMatches() {
    Timeline timeline;
    for (int winner : {-1, 2}) timeline.Capture(100, Flow::MatchResult, winner);
    timeline.Capture(0, Flow::MatchResult, 0);
    timeline.Capture(-1, Flow::MatchResult, 0);
    CHECK(timeline.Confirmed(100) == Result::None);
    timeline.Capture(32768, Flow::DrawResult, -1);
    CHECK(timeline.Confirmed(32766) == Result::None);
    CHECK(timeline.Confirmed(32767) == Result::Draw);
    timeline.Rewind(-1); // load without a GGPO identity
    CHECK(timeline.Confirmed(40000) == Result::None);
    for (int frame = 40000; frame < 40200; ++frame) {
        timeline.Capture(frame, Flow::MatchResult, 0);
        if (frame > 40000) CHECK(timeline.Confirmed(frame - 2) == Result::P1Win);
    }
    timeline.Reset();
    timeline.Capture(100, Flow::MatchResult, 0);
    timeline.Capture(100 + Timeline::Capacity, Flow::Other, -1);
    CHECK(timeline.Confirmed(1000) == Result::None); // overwritten ring slot
}

// Simulation stops at the end of the fight while the peer's inputs for the
// result frames are still in flight. No further capture happens, so the
// outcome must still be publishable when confirmation arrives later from a
// plain GGPO poll.
static void TestConfirmationAfterSimulationStops() {
    Timeline timeline;
    for (int frame = 500; frame < 540; ++frame)
        timeline.Capture(frame, frame >= 520 ? Flow::MatchResult : Flow::Other, 1);
    CHECK(timeline.Confirmed(515) == Result::None);
    CHECK(timeline.Latest().frame == 539 && timeline.Latest().result == Result::P2Win);
    CHECK(timeline.Confirmed(519) == Result::P2Win);
    CHECK(timeline.Confirmed(600) == Result::P2Win);
    timeline.Reset();
    CHECK(timeline.Latest().frame == -1 && timeline.Latest().result == Result::None);
}

int main() {
    TestResultSurvivesContinuousRollback();
    TestConfirmationAfterSimulationStops();
    TestPredictionCannotAwardWinner();
    TestRetainedTimelineAndShortResultFlow();
    TestDrawInvalidFramesAndLongMatches();
    if (failures) std::printf("%d native result test(s) failed\n", failures);
    else std::puts("Native result confirmation and continuous-rollback tests passed");
    return failures ? 1 : 0;
}
