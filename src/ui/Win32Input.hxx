#pragma once
#include <windows.h>
#include "Win32InputBridge.hxx"
#include "../common/TrainingCallInput.hxx"

// Ember's addition to the vendored Win32 backend (src/ui/backends): with a
// bridge, the window procedure's ImGui input is applied on the drawing thread.
IMGUI_API void ImGui_ImplWin32_SetInputBridge(sf4e::ui::Win32InputBridge* bridge);
// The size the game draws at, when it can differ from the window's client area:
// ImGui then lays out in those pixels and the mouse is scaled to match. Zero: the client area.
IMGUI_API void ImGui_ImplWin32_SetRenderSize(float width, float height);

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

// Go now's Enter on the call back from Training (common/TrainingCallInput.hxx),
// decided in the window procedure before Ember's menu or the game sees the
// key. A fresh press the gate takes is the call's alone, and so are its
// repeats and its character; its release goes on, which leaves nothing held
// in the game. Any other Enter goes on as it came. free: as GoNowGate::Press.
class GoNowKey {
public:
    // True for a message the caller keeps from the overlay and the game.
    bool Take(UINT message, WPARAM w, LPARAM l, input::GoNowGate& gate, bool free) {
        if (message == WM_KEYUP && w == VK_RETURN) { taken_ = false; return false; }
        if (message == WM_CHAR && w == L'\r') return taken_;
        if (message != WM_KEYDOWN || w != VK_RETURN) return false;
        // Bit 30: the key was already down, a repeat of the press before.
        if (l & (1L << 30)) return taken_;
        taken_ = gate.Press(input::GoNowGate::Source::Keyboard, free);
        return taken_;
    }
    // Focus went: the next Enter is fresh whatever came before.
    void Reset() { taken_ = false; }
private:
    bool taken_ = false;
};
} }
