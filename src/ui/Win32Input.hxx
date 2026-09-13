#pragma once
#include <windows.h>

namespace sf4e { namespace ui {
// Set before the Win32 backend's NewFrame as well as on visibility/focus changes.
// The hidden overlay must leave native cursor shape and visibility alone.
void SetOverlayCursorOwnership(bool capture);
// Nonzero means the native game must not process this message again.
LRESULT HandleOverlayMessage(HWND window, UINT message, WPARAM w, LPARAM l,
                             bool capture, bool menuAvailable);
} }
