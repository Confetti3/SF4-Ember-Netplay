#include "../common/MusicPlaybackState.hxx"
#include "test_support.hxx"

int main() {
    using namespace sf4e::music;
    LobbyPlaybackState state;
    CHECK(!state.Request(Default, true)); // Native music keeps playing untouched.
    CHECK(state.Request(1, true));
    CHECK(state.pending);
    state.pending = false;
    CHECK(!state.Request(1, true)); // No reload on each frame in the same room.
    CHECK(!state.Request(Default, false)); // Never restart menu audio in a fight.
    CHECK(state.applied == 1);
    CHECK(state.Request(2, true)); // The next safe menu accepts a new choice.
    state.pending = false;
    CHECK(state.Request(Default, true)); // Leaving restores native music.
    state.Reset(); // Native scene teardown invalidates its banks.
    CHECK(!state.pending && state.applied == Default);
    CHECK(state.Request(2, true)); // Rematch return reloads the saved theme.
    state.Reset();
    CHECK(!state.Request(2, false)); // Choice stays pending across loading.
    CHECK(state.Request(2, true));
    state.Reset();
    state.Retry();
    CHECK(!state.Request(Default, false));
    CHECK(state.Request(Default, true)); // Failed Default can be retried too.
    state.pending = false;
    CHECK(!state.Request(Default, true)); // Retry is consumed exactly once.
    const std::vector<int> installed{0, 24, CharacterBase, CharacterBase + 43};
    for (std::uint32_t roll = 0; roll < 100; ++roll) {
        CHECK(ResolveMatch(Random, roll, installed) == installed[roll % installed.size()]);
        CHECK(ResolveMatch(CharacterBase, roll, installed) == CharacterBase);
        CHECK(ResolveMatch(Default, roll, installed) == Default);
        CHECK(ResolveMatch(1, roll, installed) == Default); // Missing file excluded.
    }
    CHECK(ResolveMatch(Random, 0, {}) == Default);
    // Sakura has one cue even when the arena normally crossfades three cues.
    CHECK(BattleCueMode(CharacterBase + 23, 1) == 0);
    for (int id = 0; id < sf4e::selection::FighterCount; ++id) {
        CHECK(BattleCueMode(CharacterBase + id, 0) == 0);
        CHECK(BattleCueMode(CharacterBase + id, 1) == 0);
    }
    for (int id = 0; id < 30; ++id) if (ValidLobby(id)) {
        CHECK(BattleCueMode(id, 0) == 1); // Stage override on character-native music.
        CHECK(BattleCueMode(id, 1) == 1);
    }
    CHECK(BattleCueMode(Default, 1) == 1 && BattleCueMode(Default, 0) == 0);
    CHECK(BattleCueMode(Random, 1) == 1); // Must resolve before constructing.
    CHECK(BattleCueMode(999, 0) == 0);
}
