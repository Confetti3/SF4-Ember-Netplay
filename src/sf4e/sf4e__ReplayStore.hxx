#pragma once

#include <cstdint>
#include <string>

#include "../common/ReplayRequest.hxx"

// The game's replay table while it runs, so an archived replay put into the
// game's files (platform/ReplayFiles.hxx) shows in the native replay menu
// without a restart, and the one replay operation Ember runs on it: put a
// replay in, open the battle log, play it there, come back. Everything here
// is the game thread's.
namespace sf4e { namespace replaystore {

// Hooks Dimps::Game::ReplayInfoList::Read, which fills the table from the
// save files and is where the table is first seen. Inside the Detours
// transaction, like the other installs.
void Install();

// A replay can be put into the game: the table has been seen with the stock
// shape, and the match list has its 30 slots (sf4e__Game__Battle.hxx:
// MatchReplayListWidened), which are the ones an import writes into.
bool Ready();

// Where the operation is. One step leads to the next and every wait has an
// end, so it always comes back to Idle:
//   OpeningLog    the main menu was asked for the battle log; until its list
//                 is up, or ten seconds
//   SelectingRow  Watch: until the save controller is free and the list has
//                 the imported slot's row, which is then played the way the
//                 list's own DECIDE plays it; or ten seconds
//   Playing       the log runs the replay (its Versus and Battle states);
//                 until it is back on its list, which Ember then leaves for
//                 the main menu. Ten seconds for the replay to start, two
//                 for a battle log that went away
//   InLog         the player is in the game's own menus; until the main menu
//                 is back
enum class Step { Idle, OpeningLog, SelectingRow, Playing, InLog };

// notice: the last request's outcome for the Replays screen, an error when
// it failed. logOpens: times the battle log was opened from here, which is
// when Ember's menu gets out of the way. returns: times the main menu came
// back after that, which is when it reopens on the Replays screen.
struct Status {
	Step step = Step::Idle;
	std::string notice;
	bool noticeError = false;
	std::uint64_t logOpens = 0, returns = 0;
};
const Status& GetStatus();

// Runs a request (common/ReplayRequest.hxx: Add, Watch or OpenLog). Refused
// with a notice while another runs, or where it cannot be done: an import
// writes the game's table and files, so only at the native main menu with
// the save controller free; the battle log leaves Ember's menu, so only with
// no room.
void Start(const replay::Request& request, bool atMainMenu, bool noRoom);

// Once a game tick: moves the operation on.
void Tick(bool atMainMenu);

} }
