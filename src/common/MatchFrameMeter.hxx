#pragma once

namespace sf4e { namespace match_frame_meter {

// The frame meter over a match is for private rooms. A public room never
// shows it, whatever the player saved; the saved choice is kept for the
// private rooms they play in.
inline bool Allowed(bool publicRoom) { return !publicRoom; }

// The local player's place in a match: fighting in it, or watching it.
enum class Seat { Watching, Fighting };

// Whether a match wants the meter. A Practice room's game always does, for
// both players. Otherwise it is the player's choice for matches they watch:
// a fighter in a live match never sees it, so neither side reads frame data
// the other does not have.
inline bool Wanted(bool preference, bool practiceMatch, Seat seat) {
    return practiceMatch || (preference && seat == Seat::Watching);
}

// Whether the runtime watches matches for the meter: where a match wants it
// and it is allowed, or a replay that asks for it (a replay is not in a room).
inline bool Watched(bool wanted, bool publicRoom, bool replayMeter) {
    return (wanted && Allowed(publicRoom)) || replayMeter;
}

} }
