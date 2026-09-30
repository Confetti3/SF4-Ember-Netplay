#include "Win32Input.hxx"
#include <imgui.h>
#include <imgui_impl_win32.h>
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace sf4e { namespace ui {
// A shared flag rather than io.ConfigFlags: the window procedure sets it on
// the message thread, which may not be the drawing thread.
void SetOverlayCursorOwnership(bool capture) { SetWin32CursorOwned(capture); }
namespace {
// The button-up that ends a press, or 0 when the message is not a button press.
UINT ReleaseOf(UINT message) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: return WM_LBUTTONUP;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: return WM_RBUTTONUP;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: return WM_MBUTTONUP;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: return WM_XBUTTONUP;
    default: return 0;
    }
}
bool SamePress(UINT awaited, UINT message) { return ReleaseOf(awaited) && ReleaseOf(awaited) == ReleaseOf(message); }
}
bool ActivationClickFilter::Swallow(UINT message, LPARAM l) {
    if (message == WM_MOUSEACTIVATE) {
        // Only a click in the client area reaches the overlay; a title-bar click
        // is the system's, and leaves nothing to swallow.
        down_ = LOWORD(l) == HTCLIENT && ReleaseOf(HIWORD(l)) ? static_cast<UINT>(HIWORD(l)) : 0;
        up_ = 0;
        return false;
    }
    if (down_ && ReleaseOf(message)) {
        const bool activating = SamePress(down_, message);
        up_ = activating ? ReleaseOf(message) : 0;
        down_ = 0;
        return activating;
    }
    if (up_ && message == up_) { up_ = 0; return true; }
    return false;
}
LRESULT HandleOverlayMessage(HWND window, UINT message, WPARAM w, LPARAM l,
                             bool capture, bool menuAvailable, bool pointer) {
    const bool mouse = capture || pointer;
    SetOverlayCursorOwnership(mouse);
    const auto handled = ImGui_ImplWin32_WndProcHandler(window, message, w, l);
    const bool key = message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR;
    // In particular, WM_SETCURSOR is not in WM_MOUSEFIRST..WM_MOUSELAST.
    // Letting it fall through makes the native handler overwrite the cursor
    // just selected by ImGui on every mouse movement.
    if (mouse && handled && (capture || !key)) return handled;
    if (mouse && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) return 1;
    if (capture && key) return 1;
    if (menuAvailable && w == VK_F10 &&
        (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)) return 1;
    return 0;
}
} }
