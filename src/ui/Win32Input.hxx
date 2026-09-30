#pragma once
#include <windows.h>
#include "Win32InputBridge.hxx"

// Ember's addition to the vendored Win32 backend (src/ui/backends): with a
// bridge, the window procedure's ImGui input is applied on the drawing thread.
IMGUI_API void ImGui_ImplWin32_SetInputBridge(sf4e::ui::Win32InputBridge* bridge);

namespace sf4e { namespace ui {
// Set before the Win32 backend's NewFrame as well as on visibility/focus changes.
// The hidden overlay must leave native cursor shape and visibility alone.
void SetOverlayCursorOwnership(bool capture);
// Nonzero means the native game must not process this message again.
// `capture` takes every input (an open menu); `pointer` takes only the mouse
// and cursor (the pointer over the training HUD's chip), leaving keys and
// the pads to the game.
LRESULT HandleOverlayMessage(HWND window, UINT message, WPARAM w, LPARAM l,
                             bool capture, bool menuAvailable, bool pointer = false);

// The click that brings an inactive window forward is only a request to
// focus it. The shell stays drawn while the game is behind another window, so
// that click would land on whatever row lies under the pointer. Windows sends
// WM_MOUSEACTIVATE, naming the button message that follows, only for a click
// that activates; this swallows that press and its release and nothing else, so
// returning by keyboard costs the player no click.
class ActivationClickFilter {
public:
    // True for the press or release the caller must not deliver to the overlay.
    bool Swallow(UINT message, LPARAM l);
    // Forget a pending activation, when the window loses activation again.
    void Reset() { down_ = up_ = 0; }
private:
    UINT down_ = 0;  // the button-down message awaited after WM_MOUSEACTIVATE
    UINT up_ = 0;    // the matching button-up, once the press was swallowed
};
} }
