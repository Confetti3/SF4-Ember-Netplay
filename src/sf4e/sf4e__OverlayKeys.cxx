#include "sf4e__OverlayKeys.hxx"
#include "../common/ReplayTransport.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../ui/Win32Input.hxx"

namespace sf4e { namespace overlay {
bool KeepsKeyFromGame(UINT message, WPARAM w, LPARAM l, const KeyOwners& owners) {
    // The position hotkeys only as plain keys, so Alt+F4 still reaches the game.
    const bool plainKey = message == WM_KEYDOWN || message == WM_KEYUP, systemKey = message == WM_SYSKEYDOWN || message == WM_SYSKEYUP;
    if (replaytransport::KeepsKey(owners.replayPlayback, message, static_cast<unsigned>(w))) return true;
    if (ui::KeepsAltEnter(message, w, l, owners.watchingStream)) return true;
    return owners.trainingAvailable && !owners.replayPlayback && ((w >= VK_F5 && w <= VK_F8 && (plainKey || systemKey)) ||
        (plainKey && w >= VK_F1 && w <= VK_F12 && ui::TrainingHotkeyBound(static_cast<int>(w - VK_F1))));
}
} }
