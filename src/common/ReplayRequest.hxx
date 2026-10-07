#pragma once

// What the Replays screen, or a replay link the player agreed to, asks of
// the game: one path and one thing to do with it. The shell makes it, the
// runtime carries it to the game thread, and sf4e__ReplayStore runs it.

#include <string>

namespace sf4e { namespace replay {

// Add: put the archived replay at `path` into the game's replay list.
// Watch: that, then open the game's battle log and play it.
// Export: Watch, with the playback written to an .mp4 beside the replay by
// Ember's encoder.
// OpenLog: open the battle log; no path.
// DismissLink: the player declined the replay a link asked for; no path.
enum class Mode { None, Add, Watch, Export, OpenLog, DismissLink };

// What an export draws over the game while it records, on screen and so in
// the video: each part on or off, with the text the player left in it.
// names: each player's name on a plate over the game's own PLAYER label,
// with that player's wins of the set beside it when set is on. line: one
// line over the timer. mark: Ember's name in a corner.
struct Caption {
	bool names = false, line = false, set = false, mark = false;
	std::string name[2], text;
	int wins[2] = {0, 0};
	bool Any() const { return names || line || set || mark; }
};

struct Request {
	Mode mode = Mode::None;
	std::string path;
	// With Export.
	Caption caption;
};

} }
