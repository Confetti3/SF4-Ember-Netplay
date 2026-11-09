#pragma once

// Launcher messages for a product's own launch steps (LaunchSteps.hxx): their
// words are in the shared catalogs, which a product's overlay can only
// re-word, so their ids live here, beside the launcher that shows them. SF4
// Ember's steps use none of them.

namespace ember { namespace launch { namespace messages {

// Steam never came up with an account signed in (no arguments).
constexpr const char* SteamNotReady = "launcher.steam_not_ready";
// A restart of the player's running Steam, asked first (AskOkCancel).
constexpr const char* SteamRestartAsk = "launcher.steam_restart_ask";
// The executable is not the game build the product runs on: {0} the
// product, {1} the build it runs on, {2} the file found.
constexpr const char* GameBuildUnsupported = "launcher.game_build_unsupported";

} } }
