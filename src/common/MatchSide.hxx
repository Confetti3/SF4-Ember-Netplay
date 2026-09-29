#pragma once
#include <cstdint>
#include <string>

#include "NetworkLink.hxx"

namespace sf4e {
// One fighter of a native match as the HUD shows it. Name and link are taken
// together when the match starts, so a room that has moved on (a new pair at
// the table while a spectator still watches the old game) cannot put one
// player's link beside another player's name.
struct MatchSide {
    std::string name;
    NetworkLink link = NetworkLink::Unknown;
};

// The set score the HUD shows beside a match's names. A fighter sees the
// table's running score. A spectator's stream can lag the room by seconds, so
// the room may already have counted the game it is still watching, or reset the
// score when the loser left; it keeps the score the game started with, the same
// way the names are kept. False when there is no score to show.
inline bool HudScore(bool spectating, bool startKnown, const std::uint32_t (&start)[2],
    bool liveKnown, const std::uint32_t (&live)[2], std::uint32_t (&out)[2]) {
    const bool known = spectating ? startKnown : liveKnown;
    const std::uint32_t* shown = spectating ? start : live;
    if (known) { out[0] = shown[0]; out[1] = shown[1]; }
    return known;
}
}
