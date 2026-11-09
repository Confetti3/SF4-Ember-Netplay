#pragma once
// A game for the runtime's tests, defined in fake_game.cxx: ember::game::Game()
// without SF4. A test sets what the game offers and where it is, and reads
// back what the runtime asked of it.
#include "../sf4e/sf4e__GameAdapter.hxx"

namespace ember { namespace test {

struct FakeGame {
    // GameState::MainMenuInFront.
    bool mainMenu = true;
    // What Battle::Enter answers.
    game::Battle::Entry entry = game::Battle::Entry::Deferred;
    // GameState::MissingStages.
    std::vector<std::int64_t> missingStages;
    // The calls the runtime made.
    int trainingRequests = 0;
    int battleEntries = 0;
};

FakeGame& Fake();
// The capabilities Game() answers with.
game::Capabilities& FakeCapabilities();
// Every field of Fake() back to its default, and every capability on, so a test
// turns off the one it examines. The fake lists them itself: it is not SF4's set.
void ResetFake();

} } // namespace ember::test
