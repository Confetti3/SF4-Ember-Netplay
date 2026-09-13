// Drive the real Win32 backend and overlay message route on a hidden window.
// A game-style fallback clears the OS cursor if overlay handling falls through.
#include "../ui/Win32Input.hxx"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <cstdio>

int main() {
    HWND window = CreateWindowW(L"STATIC", L"Ember cursor regression", WS_OVERLAPPEDWINDOW,
        0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 2;
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(window);
    ImGui_ImplWin32_NewFrame();
    int disappeared = 0;
    int failures = 0;
    const auto check = [&](bool condition, const char* description) {
        if (!condition) { ++failures; std::printf("FAIL: %s\n", description); }
    };
    const auto previous = GetCursor();
    for (int movement = 0; movement < 100; ++movement) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        const auto handled = sf4e::ui::HandleOverlayMessage(window, WM_SETCURSOR,
            reinterpret_cast<WPARAM>(window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE), true, true);
        if (!handled) SetCursor(nullptr); // Native game receives the same message.
        if (!GetCursor()) ++disappeared;
    }
    check(disappeared == 0, "native fallback hid the owned cursor");
    const auto arrow = LoadCursor(nullptr, IDC_ARROW);
    const auto text = LoadCursor(nullptr, IDC_IBEAM);
    const auto resize = LoadCursor(nullptr, IDC_SIZEWE);
    for (int transition = 0; transition < 20; ++transition) {
        // Closure, gameplay and loss of focus all release cursor ownership.
        sf4e::ui::SetOverlayCursorOwnership(false);
        SetCursor(resize);
        ImGui::SetMouseCursor(transition % 2 ? ImGuiMouseCursor_TextInput : ImGuiMouseCursor_Arrow);
        ImGui_ImplWin32_NewFrame();
        check(GetCursor() == resize, "hidden overlay changed native cursor during NewFrame");
        check(sf4e::ui::HandleOverlayMessage(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
            MAKELPARAM(HTCLIENT, WM_MOUSEMOVE), false, true) == 0, "hidden overlay swallowed native cursor event");
        check(GetCursor() == resize, "hidden overlay changed native cursor during message handling");
        // Reopening must restore both the arrow and text-input cursor reliably.
        sf4e::ui::SetOverlayCursorOwnership(true);
        ImGui_ImplWin32_NewFrame();
        check(sf4e::ui::HandleOverlayMessage(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
            MAKELPARAM(HTCLIENT, WM_MOUSEMOVE), true, true) != 0, "reopened overlay lost cursor event ownership");
        check(GetCursor() == (transition % 2 ? text : arrow), "wrong cursor after reopening");
        SetCursor(resize);
        check(sf4e::ui::HandleOverlayMessage(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
            MAKELPARAM(HTLEFT, WM_MOUSEMOVE), true, true) == 0, "overlay swallowed window-border cursor");
        check(GetCursor() == resize, "overlay replaced window-border cursor");
    }
    check(sf4e::ui::HandleOverlayMessage(window, WM_KEYDOWN, 'A', 0, true, true) != 0, "menu typing leaked");
    check(sf4e::ui::HandleOverlayMessage(window, WM_KEYUP, 'A', 0, false, false) == 0, "native key was swallowed");
    check(sf4e::ui::HandleOverlayMessage(window, WM_SYSKEYDOWN, VK_F10, 0, false, true) != 0, "F10 was not consumed at menu");
    check(sf4e::ui::HandleOverlayMessage(window, WM_SYSKEYDOWN, VK_F10, 0, false, false) == 0, "F10 was consumed during gameplay");
    SetCursor(previous);
    ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); DestroyWindow(window);
    std::printf("Cursor disappeared after %d / 100 overlay mouse events\n", disappeared);
    if (!failures) std::puts("Cursor ownership, 20 visibility transitions, text cursor, native borders and keyboard routing passed");
    return failures ? 1 : 0;
}
