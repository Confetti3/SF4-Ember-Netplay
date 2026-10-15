#pragma once

namespace sf4e { namespace match_frame_meter {

// The frame meter over a match is for private rooms. A public room never
// shows it, whatever the player saved; the saved choice is kept for the
// private rooms they play in.
inline bool Allowed(bool publicRoom) { return !publicRoom; }

// Whether a match wants the meter: the player's choice, or a Practice room's
// game, which shows it whatever that choice is.
inline bool Wanted(bool preference, bool practiceMatch) { return preference || practiceMatch; }

// Whether the runtime watches matches for the meter: where a match wants it
// and it is allowed, or a replay that asks for it (a replay is not in a room).
inline bool Watched(bool wanted, bool publicRoom, bool replayMeter) {
    return (wanted && Allowed(publicRoom)) || replayMeter;
}

} }
