#pragma once

// What the Replays screen, or a replay link the player agreed to, asks of
// the game: one path and one thing to do with it. The shell makes it, the
// runtime carries it to the game thread, and sf4e__ReplayStore runs it.

#include <string>

namespace sf4e { namespace replay {

// Add: put the archived replay at `path` into the game's replay list.
// Watch: that, then open the game's battle log and play it.
// OpenLog: open the battle log; no path.
// DismissLink: the player declined the replay a link asked for; no path.
enum class Mode { None, Add, Watch, OpenLog, DismissLink };

struct Request {
	Mode mode = Mode::None;
	std::string path;
};

} }
