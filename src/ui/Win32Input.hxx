#pragma once
#include <windows.h>

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
} }
