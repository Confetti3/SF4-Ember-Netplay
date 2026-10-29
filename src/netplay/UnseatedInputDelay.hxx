#pragma once
#include "SessionController.hxx"
#include "../session/RoomModel.hxx"

namespace sf4e { namespace netplay {
// Rechecked against the current room on the game thread: a queued preference
// must not become a fighter's delay change if a seat was granted meanwhile.
inline bool CanSaveUnseatedInputDelay(const Snapshot& session, const room::Snapshot& room) {
    return session.room == RoomState::Joined && !session.readyPending &&
        room.roomEpoch && room.localMember && !room.closed && room::FindMember(room, room.localMember) &&
        room::PlaceOf(room, room.localMember).kind != room::Place::Kind::Seat;
}
} }
