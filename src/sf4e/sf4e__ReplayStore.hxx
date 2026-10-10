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
//   PreparingImport worker preparation; commit only while native save/table
//                 revision and file/account notification guards remain fresh
//   OpeningLog    the main menu was asked for the battle log; until its list
//                 is up, or ten seconds
//   SelectingRow  Watch: until the save controller is free and the list has
//                 the imported slot's row, which is then played the way the
//                 list's own DECIDE plays it; or ten seconds
//   Playing       the log runs the replay (its Versus and Battle states);
//                 until it is back on its list, which Ember then leaves for
//                 the main menu. Ten seconds for the replay to start, thirty
//                 for its Versus screen to reach the battle, two for a
//                 battle log that went away
//   InLog         the player is in the game's own menus; until the main menu
//                 is back, or they are in a room
enum class Step { Idle, PreparingImport, OpeningLog, SelectingRow, Playing, InLog };

// notice: the last request's outcome for the Replays screen, an error when
// it failed. logOpens: times the battle log was opened from here, which is
// when Ember's menu gets out of the way. returns: times the main menu came
// back after that, which is when it reopens on the Replays screen.
struct Status {
	Step step = Step::Idle;
	std::string notice;
	bool noticeError = false;
	std::uint64_t logOpens = 0, returns = 0;
	// The running export's caption, and whether it is to be drawn now
	// (while the replay's battle records).
	replay::Caption caption;
	bool captionShown = false;
};
const Status& GetStatus();
// A playback that is being made into a video is running: the game is then
// kept playing and sounding behind another window (sf4e__BackgroundPlay.cxx).
bool Exporting();
// The replay that is playing was asked for with the frame meter: each of its
// frames is then shown to the meter (sf4e__Game__Battle__System.cxx).
bool MeterWanted();
// The archived file a Watch request is playing, while it plays; empty for
// an export and for a replay the player started from the game's own list.
const std::string& PlayingFile();

// Runs a request (common/ReplayRequest.hxx: Add, Watch, Export or OpenLog). An export records from the Battle state to the log's return
// (sf4e__ReplayCapture.hxx) and its outcome becomes the notice. Refused
// with a notice while another runs, or where it cannot be done: an import
// writes the game's table and files, so only at the native main menu with
// the save controller free; the battle log leaves Ember's menu, so only with
// no room.
void Start(const replay::Request& request, bool atMainMenu, bool noRoom);

// Once a game tick: moves the operation on. noRoom as for Start: an operation
// whose player has gone into a room ends without reopening Ember's menu.
void Tick(bool atMainMenu, bool noRoom);

} }
