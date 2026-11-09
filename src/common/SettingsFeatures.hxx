#pragma once
// The game features a Settings row sets that only some games have. A game's
// adapter names the ones it has (game::Capabilities::settingsFeatures); the
// shell leaves out the rows of the rest (ShellView::settingsFeatures and the
// Settings screens' one table of such rows). Every flag is off until the game
// sets it, by name; All() is SF4's, and the shell's default.

namespace ember {

struct SettingsFeatures {
    // Ember's fixes to how the game reads input and shows its frames: the
    // Player page's Lower input lag, Lower keyboard lag, Faster display and
    // Borderless window.
    bool inputDisplayFixes = false;
    // Ember's pad layer, which reads every controller for the game: the
    // Player page's Controllers, Button layout, Game's button settings and
    // Game reads controllers rows. The pad wizard inside it is
    // Capabilities::padSetup.
    bool padLayer = false;
    // The keyboard as a gameplay device: the Player page's Use keyboard.
    bool keyboard = false;
    // The announcer's call as an opponent readies (GameState::PlayChallengerCall):
    // Sounds' Ready sound, its volume and its test.
    bool challengerCall = false;
    // The game's music in a room (Services::UseLobbyMusic): Sounds' Lobby music.
    bool lobbyMusic = false;
    // Full-length fighter pictures beside the portraits: Room layout's Pictures.
    bool characterPictures = false;
    // "F10 or Start" as Ember's menu closes: Help's Show shortcut hint.
    bool shortcutHint = false;

    static constexpr SettingsFeatures All() {
        SettingsFeatures all;
        all.inputDisplayFixes = all.padLayer = all.keyboard = all.challengerCall = true;
        all.lobbyMusic = all.characterPictures = all.shortcutHint = true;
        return all;
    }
};

} // namespace ember
