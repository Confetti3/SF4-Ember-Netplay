#include "Win32Input.hxx"
#include <imgui.h>
#include <imgui_impl_win32.h>
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace sf4e { namespace ui {
void SetOverlayCursorOwnership(bool capture) {
    auto& io = ImGui::GetIO();
    if (capture) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
}
LRESULT HandleOverlayMessage(HWND window, UINT message, WPARAM w, LPARAM l,
                             bool capture, bool menuAvailable) {
    SetOverlayCursorOwnership(capture);
    const auto handled = ImGui_ImplWin32_WndProcHandler(window, message, w, l);
    // In particular, WM_SETCURSOR is not in WM_MOUSEFIRST..WM_MOUSELAST.
    // Letting it fall through makes the native handler overwrite the cursor
    // just selected by ImGui on every mouse movement.
    if (capture && handled) return handled;
    if (capture && ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
        message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR)) return 1;
    if (menuAvailable && w == VK_F10 &&
        (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP)) return 1;
    return 0;
}
} }
