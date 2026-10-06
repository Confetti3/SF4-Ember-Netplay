#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The game's replay table while it runs, so an archived replay put into the
// game's files (platform/ReplayFiles.hxx) shows in the native replay menu
// without a restart.
namespace sf4e { namespace replaystore {

// Hooks Dimps::Game::ReplayInfoList::Read, which fills the table from the
// save files and is where the table is first seen. Inside the Detours
// transaction, like the other installs.
void Install();

// The table has been seen and has the stock shape.
bool Ready();

enum class Outcome { Added, NotReady, Failed };

// A replay and its slot record as put into the game, for TickPlayback.
struct Playable {
	int slot = -1;
	std::vector<std::uint8_t> replay, record;
};

// Puts an archived replay into the game's files and its table, as the newest
// entry of the match list. On the game thread, between battles: a battle
// that ends saves its own replay into the table. With playable, what
// TickPlayback needs.
Outcome Import(const std::wstring& path, Playable* playable = nullptr);

// Plays a replay in the battle log once it is the foreground event, the way
// the game's own replay player does (0x483600, 0x482CE0): the save controller
// reads the slot into a buffer through Steam, the replay system takes the
// bytes (ReplaySystem+0x50, 0x5D6250, which reads them during the call), a
// battle request is built from the replay's header and record and handed to
// the battle flow, and the log's event controller is asked for its "Versus"
// state, which loads that request, runs the replay battle and comes back to
// "Select". Call each game tick after the jump; true once it is started or
// given up on.
bool TickPlayback(const Playable& playable);

// After TickPlayback, true once the battle log is back on its list: the
// replay was watched (or left), and the log can be left too.
bool PlaybackOver();

} }
