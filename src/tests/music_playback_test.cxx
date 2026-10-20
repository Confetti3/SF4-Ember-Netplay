#include "../common/MusicPlaybackState.hxx"
#include "test_support.hxx"
#include <algorithm>

int main() {
    using namespace sf4e::music;
    // Saved menu music starts before a room exists. Entering/leaving a room
    // uses the same MainMenu bank without restarting it on every page.
    MenuPlaybackState startup;
    const int savedTheme = CharacterBase + 23;
    CHECK(!startup.Request(savedTheme, MenuPlaybackAllowed("", false)));
    CHECK(startup.Request(savedTheme, MenuPlaybackAllowed("Title", false)));
    startup.pending = false;
    for (const auto screen : {"MainMenu", "Option", "PlayerData",
                              "PlayerTitleSetting", "PlayerIconSetting", "Manual", "Marketplace"}) {
        CHECK(MenuPlaybackAllowed(screen, false));
        CHECK(!startup.Request(savedTheme, MenuPlaybackAllowed(screen, false)));
        CHECK(!MenuPlaybackAllowed(screen, true));
    }
    for (const auto screen : {"", "Boot", "LogoCapcom", "Opening", "DemoBattle", "VSMode",
                              "TrainingMode", "ArcadeMode", "TrialMode", "LocalBattleLog", "ReplayChannel", "Unknown"})
        CHECK(!MenuPlaybackAllowed(screen, false));
    CHECK(!MenuPlaybackAllowed("Title", true));
    startup.Reset(); // The native fight owns teardown; return loads the saved menu bank.
    CHECK(!startup.Request(savedTheme, MenuPlaybackAllowed("VSMode", false)));
    CHECK(startup.Request(savedTheme, MenuPlaybackAllowed("MainMenu", false)));
    startup.Reset();
    CHECK(startup.Request(savedTheme, MenuPlaybackAllowed("Title", false))); // Next launch.

    MenuPlaybackState state;
    CHECK(!state.Request(Default, true)); // Native music keeps playing untouched.
    CHECK(state.Request(1, true));
    CHECK(state.pending);
    state.pending = false;
    CHECK(!state.Request(1, true)); // No reload on each frame in the same menu.
    CHECK(!state.Request(Default, false)); // Never restart menu audio in a fight.
    CHECK(state.applied == 1);
    CHECK(state.Request(2, true)); // The next safe menu accepts a new choice.
    state.pending = false;
    CHECK(state.Request(Default, true)); // Resetting the preference restores native music.
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
    const Choices available({143, 0, 999, 0, 100});
    CHECK(available.ids == std::vector<int>({Default, Random, 0, 100, 143}));
    CHECK(available.Step(0, 1) == 100); // Missing banks cannot be stepped onto.
    CHECK(available.FromKey("24") == Default && available.FromKey("100") == 100);
    CHECK(available.FromKey("100oops") == Default);
    CHECK(Choices().ids == std::vector<int>({Default, Random}));
    MenuPlaybackState randomMenu;
    CHECK(!randomMenu.RequestChoice(Random, 0, installed, false));
    CHECK(randomMenu.RequestChoice(Random, 0, installed, true));
    CHECK(randomMenu.applied == installed[0]); randomMenu.pending = false;
    CHECK(!randomMenu.NeedsChoice(Random, true)); // No per-frame RNG or reload.
    CHECK(!randomMenu.RequestChoice(Random, 1, installed, true));
    CHECK(randomMenu.applied == installed[0]);
    randomMenu.Reset(); // Native menu-bank release after a match.
    CHECK(randomMenu.RequestChoice(Random, 1, installed, true));
    CHECK(randomMenu.applied == installed[1]);
    randomMenu.Reset();
    CHECK(!randomMenu.RequestChoice(Default, 2, installed, true)); // Default stays native.

    // Each game prepares exactly once, despite repeated ticks or a stale
    // observation. A confirmed result alone must not change victory music.
    RematchMusicState cycle;
    CHECK(!cycle.Observe(0, true));
    CHECK(!cycle.Observe(1, false));
    CHECK(!cycle.MayApply());
    CHECK(cycle.Observe(1, true));
    CHECK(!cycle.MayApply());
    CHECK(!cycle.Observe(1, true));
    CHECK(!cycle.Observe(2, false));
    CHECK(cycle.MayApply());
    CHECK(!cycle.Observe(1, true)); // Never rewind local audio for older frames.
    CHECK(cycle.current == 2 && cycle.preparedAfter == 1);
    CHECK(cycle.Observe(2, true));
    CHECK(!cycle.MayApply());
    CHECK(!cycle.Observe(3, false));
    CHECK(cycle.MayApply());
    CHECK(cycle.Observe(3, true));
    CHECK(!cycle.Observe(3, true));
    CHECK(NextRandomTrack(0, 0, {}) == Default);
    CHECK(NextRandomTrack(0, 1, {0}) == 0);
    CHECK(NextRandomTrack(0, 3, {24}) == 24);
    for (const int current : installed) {
        for (std::uint32_t roll = 0; roll < 100; ++roll) {
            const auto next = NextRandomTrack(current, roll, installed);
            CHECK(next != current);
            CHECK(std::find(installed.begin(), installed.end(), next) != installed.end());
        }
    }
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
    // A custom stage's own bank and cue layout win over every local override.
    for (const auto& track : TrackList()) {
        CHECK(BattleChoice(track.id, false) == track.id);
        CHECK(BattleChoice(track.id, true) == Default);
        CHECK(BattleCueMode(BattleChoice(track.id, true), 0) == 0);
        CHECK(BattleCueMode(BattleChoice(track.id, true), 1) == 1);
    }
}
