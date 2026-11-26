#pragma once
// The keys the overlay's window procedure keeps from the game once Ember's menu
// has had them (HandleOverlayMessage): a replay's transport keys, Alt+Enter over
// a watched match, and Training's F5 to F8 and the player's bound F keys.
#include <windows.h>

namespace sf4e { namespace overlay {
struct KeyOwners {
    bool replayPlayback = false, watchingStream = false, trainingAvailable = false;
};
bool KeepsKeyFromGame(UINT message, WPARAM w, LPARAM l, const KeyOwners& owners);
} }
