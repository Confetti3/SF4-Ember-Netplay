#pragma once
// The keys the overlay's window procedure keeps from the game once Ember's menu
// has had them (HandleOverlayMessage): a replay's keys, Alt+Enter over a
// watched match, and Training's F5 to F8 and the player's bound F keys; and
// what a replay's keys ask for.
#include <cstdint>
#include <windows.h>

namespace sf4e { namespace overlay {
struct KeyOwners {
    bool replayPlayback = false, watchingStream = false, trainingAvailable = false;
};
bool KeepsKeyFromGame(UINT message, WPARAM w, LPARAM l, const KeyOwners& owners);
// The replay's keys pressed this frame, as commands for the playback session
// named (ReplayTransport.hxx: PlaybackKey): F1 to F5 and F9 the transport's
// and the layers', F6 to F8 takeover's.
void SendReplayKeys(std::uint32_t session);
} }
