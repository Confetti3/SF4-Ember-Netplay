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
