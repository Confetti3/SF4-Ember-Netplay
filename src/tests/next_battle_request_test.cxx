// The seed and Training rules of a room match reach the battle the game
// prepares for that match, and only that battle.
#include "../common/NextBattleRequest.hxx"

#include "test_support.hxx"
using sf4e::battle::NextRequest;

namespace {
// The order the game runs a room match in: the runtime enters it
// (StartMatchFromLobby), the game prepares the battle request
// (PrepareBattleRequest), and only then does the battle register the tasks
// that start GGPO, too late to give the request anything.
struct Game {
    NextRequest pending;
    void Enter(bool training, std::uint32_t seed) { pending = NextRequest::ForMatch(training, seed); }
    void Defer() { pending = {}; }
    NextRequest Prepare() { return pending.Take(); }
};
}

int main() {
    Game game;
    // A Practice match's battle is prepared with Training rules and the
    // match's own seed.
    game.Enter(true, 1234);
    auto battle = game.Prepare();
    CHECK(battle.training && battle.overrideSeed && battle.seed == 1234);
    // The match after it plays by its own rules and seed; neither carries over.
    game.Enter(false, 5678);
    battle = game.Prepare();
    CHECK(!battle.training && battle.overrideSeed && battle.seed == 5678);
    // A battle the game prepares with no match entered (offline, or a second
    // preparation) gets nothing of the last match.
    battle = game.Prepare();
    CHECK(!battle.training && !battle.overrideSeed && battle.seed == NextRequest().seed);
    // A start that is deferred after a Practice match was entered but never
    // prepared leaves nothing for the next battle.
    game.Enter(true, 99);
    game.Defer();
    battle = game.Prepare();
    CHECK(!battle.training && !battle.overrideSeed);
    // A seed of all ones is still the match's, not the absence of one.
    game.Enter(false, 0xffffffff);
    battle = game.Prepare();
    CHECK(battle.overrideSeed && battle.seed == 0xffffffff);
    std::puts("Next battle request journeys passed.");
    return 0;
}
