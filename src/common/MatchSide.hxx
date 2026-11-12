#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "NetworkLink.hxx"
#include "SkillBadge.hxx"

namespace sf4e {
// One fighter of a native match as the HUD shows it. Name, link and badge are
// taken together when the match starts, so a room that has moved on (a new
// pair at the table while a spectator still watches the old game) cannot put
// one player's link or class beside another player's name.
struct MatchSide {
    std::string name;
    NetworkLink link = NetworkLink::Unknown;
    // The class mark as the room shows it on this player's screen (the
    // runtime's shown room): none outside a public room this client joined as
    // public, and none while the player hides class emblems.
    SkillBadge badge;
};

// Both fighters of a native match as the HUD shows them, taken when it starts.
// `lobby` is the match's native roster (members with a name): the two seats
// first, then any spectators, so a fighter and a spectator of the same game
// read the same two sides. A seat with no name is left empty. Room names are
// unique, so the name finds each fighter's link in `room` (the client's own
// room) and class in `shown` (the room as the runtime shows it, which keeps
// a class only in a room joined as public and only while class emblems are
// shown; null when there is none). The client's own room is never read for a
// class: a private room's host writes it.
template <class Lobby, class Members>
void CaptureMatchSides(const Lobby& lobby, const Members& room, const Members* shown, MatchSide (&sides)[2]) {
    auto seat = lobby.begin();
    for (std::size_t side = 0; side < 2; ++side) {
        auto& captured = sides[side];
        captured = MatchSide();
        if (seat == lobby.end()) continue;
        captured.name = seat->name;
        ++seat;
        if (captured.name.empty()) continue;
        for (const auto& member : room)
            if (member.name == captured.name) captured.link = member.link;
        if (shown)
            for (const auto& member : *shown)
                if (member.name == captured.name) captured.badge = member.badge;
    }
}

// The set score the HUD shows beside a match's names. A fighter sees the
// table's running score. A spectator's stream can lag the room by seconds, so
// the room may already have counted the game it is still watching, or reset the
// score when the loser left; it shows the score its own stream has reached (see
// CountWatchedGame), the same way the names are kept. False when there is no
// score to show.
inline bool HudScore(bool spectating, bool watchedKnown, const std::uint32_t (&watched)[2],
    bool liveKnown, const std::uint32_t (&live)[2], std::uint32_t (&out)[2]) {
    const bool known = spectating ? watchedKnown : liveKnown;
    const std::uint32_t* shown = spectating ? watched : live;
    if (known) { out[0] = shown[0]; out[1] = shown[1]; }
    return known;
}

// When both fighters choose Rematch over the WIN, the battle restarts in place:
// the room counts the game, but the match and a spectator's stream go on. One
// stream can then show a whole set, so a spectator's score starts at the
// table's score and counts each game the stream shows ending, for the side that
// won it (0 or 1; anything else, such as a draw, counts nothing).
inline void CountWatchedGame(int winnerSide, std::uint32_t (&score)[2]) {
    if (winnerSide == 0 || winnerSide == 1) ++score[winnerSide];
}
}
