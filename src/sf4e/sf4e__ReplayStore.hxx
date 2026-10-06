#pragma once

#include <string>

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

// Puts an archived replay into the game's files and its table, as the newest
// entry of the match list. On the game thread, between battles: a battle
// that ends saves its own replay into the table.
Outcome Import(const std::wstring& path);

} }
