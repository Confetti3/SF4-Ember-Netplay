#pragma once
// What a BluMint room says when a game or a set of a BluMint match ends, and
// what it offers next. BluMint runs the event (king of the hill, streamer vs
// viewer, double elimination; docs/design/BLUMINT_ROOM.md) and tells Ember
// none of that: Ember knows the two fighters, the set length, the score, and
// how the match ended. So the words are the same for every format, and the
// round's own label says the rest when BluMint gave one.
#include "GameMenu.hxx"
#include <string>

namespace sf4e { namespace ui {
enum class BluMintMoment {
    GameDone,   // a game ended and the set goes on (first to N not reached)
    SetDone,    // the set is won; BluMint has the result
    Restart,    // a game was cut off (a lost connection); BluMint sets the match up again from the score
    HostTook,   // BluMint's host closed the match: took it over, entered a score or removed it
    Expired,    // nobody played it in time; BluMint's host decides
    Called,     // a BluMint match is ready for this player; BluMint disqualifies one not ready in time
    Failed,     // Ember could not carry the match on (`failure` says why); BluMint's host settles it
};
struct BluMintScene {
    BluMintMoment moment = BluMintMoment::GameDone;
    std::string names[2], round;
    unsigned wins[2] = {0, 0};
    int gamesToWin = 0;
    // The fighter on this PC (0 or 1), or -1 for a watcher, who gets no buttons.
    int you = -1;
    // The fighter's next BluMint match is already waiting, against `next`.
    std::string next;
    // Called: the player is in another room, which playing the match leaves.
    bool inRoom = false;
    // Failed: why, in the player's words.
    std::string failure;
};
// The notice for `scene`: its heading, its words and its buttons. Results:
// BluMintReady (ready for the next game), BluMintFighter (change fighter
// first), BluMintNext (play the next or called match), Acknowledged (close).
// The match's room closes by itself when the match ends, so no notice offers
// to stay in it or leave it.
NoticeDialog BluMintNotice(const BluMintScene& scene);
} }
