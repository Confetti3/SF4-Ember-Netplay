#pragma once
#include "SessionController.hxx"
#include "PlayerPreferences.hxx"
#include "../session/RoomModel.hxx"

namespace sf4e { namespace netplay {
// Rechecked against the current room on the game thread: a queued preference
// must not become a fighter's delay change if a seat was granted meanwhile.
inline bool CanSaveUnseatedInputDelay(const Snapshot& session, const room::Snapshot& room) {
    return session.room == RoomState::Joined && !session.readyPending &&
        room.roomEpoch && room.localMember && !room.closed && room::FindMember(room, room.localMember) &&
        room::PlaceOf(room, room.localMember).kind != room::Place::Kind::Seat;
}
// Copy and save the current preferences, never a UI copy of the whole profile.
inline bool ChooseInputDelay(PlayerPreferences& preferences, int selected) {
    if (selected != AutoInputDelayChoice && (selected < MinimumInputDelay || selected > MaximumInputDelay)) return false;
    preferences.autoInputDelay = selected == AutoInputDelayChoice;
    if (!preferences.autoInputDelay) preferences.inputDelay = selected;
    return true;
}
} }
