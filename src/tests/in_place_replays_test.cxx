// Which games of a battle with in-place rematches Ember keeps the replay of:
// every game after the first, which the game saves itself, exactly once.
#include <cstdio>
#include <functional>
#include "../common/InPlaceReplays.hxx"
#include "test_support.hxx"

using sf4e::replay::InPlaceReplays;
using Bytes = InPlaceReplays::Bytes;

// The recorder as it is when asked: the game it holds, and how often it was asked.
struct Recorder {
    Bytes game;
    int built = 0;
    Bytes operator()() { ++built; return game; }
};
static Bytes Game(std::uint8_t number) { return Bytes(4, number); }

// A first-to-3 set played in one battle: games 2 and 3 are Ember's to keep.
static void TestEveryGameAfterTheFirstIsKept() {
    InPlaceReplays replays;
    Recorder recorder;
    recorder.game = Game(1);
    replays.Restarting(std::ref(recorder));
    EXPECT(recorder.built == 0);                  // the game saved the first itself
    EXPECT(replays.Continued().empty());
    recorder.game = Game(2);
    replays.Restarting(std::ref(recorder));
    recorder.game = Game(3);                      // the next game records over it
    EXPECT(replays.Continued() == Game(2));
    EXPECT(replays.Closed(std::ref(recorder)) == Game(3));
    EXPECT(recorder.built == 2);
}

// A battle of one game is the game's to save, and so is a first game whose
// rematch never got going.
static void TestFirstGameIsTheGames() {
    InPlaceReplays replays;
    Recorder recorder{Game(1)};
    EXPECT(replays.Closed(std::ref(recorder)).empty());
    replays.Restarting(std::ref(recorder));
    recorder.game = Game(2);                      // the next game's first round
    EXPECT(replays.Closed(std::ref(recorder)).empty());
    EXPECT(recorder.built == 0);
}

// The battle closes after a restart that never became final (a rematch
// rolled back, or the next game cut before its fight): the game that ended
// is kept from the restart, not from what the recorder holds by then.
static void TestRestartThatNeverWentOn() {
    InPlaceReplays replays;
    Recorder recorder{Game(1)};
    replays.Restarting(std::ref(recorder));
    replays.Continued();
    recorder.game = Game(2);
    replays.Restarting(std::ref(recorder));
    replays.Restarting(std::ref(recorder));       // passed again after a rollback
    recorder.game = Game(3);
    EXPECT(replays.Closed(std::ref(recorder)) == Game(2));
    EXPECT(recorder.built == 2);
}

// Each battle starts again from its own first game.
static void TestResetStartsTheNextBattle() {
    InPlaceReplays replays;
    Recorder recorder{Game(1)};
    replays.Restarting(std::ref(recorder));
    replays.Continued();
    recorder.game = Game(2);
    EXPECT(replays.Closed(std::ref(recorder)) == Game(2));
    replays.Reset();
    recorder.game = Game(7);
    EXPECT(replays.Closed(std::ref(recorder)).empty());
}

// A battle left without its close (a leftover session retired as the next
// one begins) is reset with the next session: nothing it held or counted
// reaches the next battle, whose first game is the game's own again.
static void TestLeftoverBattleStartsClean() {
    InPlaceReplays replays;
    Recorder recorder{Game(1)};
    replays.Restarting(std::ref(recorder));
    replays.Continued();
    recorder.game = Game(2);
    replays.Restarting(std::ref(recorder));       // held, never handed over
    replays.Reset();                              // the next session begins
    recorder.game = Game(8);
    replays.Restarting(std::ref(recorder));
    EXPECT(replays.Continued().empty());
    EXPECT(recorder.built == 1);
    recorder.game = Game(9);
    EXPECT(replays.Closed(std::ref(recorder)) == Game(9));
}

int main() {
    TestEveryGameAfterTheFirstIsKept();
    TestFirstGameIsTheGames();
    TestRestartThatNeverWentOn();
    TestResetStartsTheNextBattle();
    TestLeftoverBattleStartsClean();
    if (TEST_FAILURES()) std::printf("%d in-place replay test(s) failed\n", TEST_FAILURES());
    else std::puts("In-place replay tests passed");
    return TEST_EXIT_CODE();
}
