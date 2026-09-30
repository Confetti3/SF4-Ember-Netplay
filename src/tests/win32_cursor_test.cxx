// Drive the real Win32 backend and overlay message route on a hidden window.
// A game-style fallback clears the OS cursor if overlay handling falls through.
#include "../ui/Win32Input.hxx"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <atomic>
#include <cstdio>
#include <thread>
IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

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
    // Over the training chip the mouse is Ember's, and the keys stay the game's.
    sf4e::ui::SetOverlayCursorOwnership(false);ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);ImGui_ImplWin32_NewFrame();
    check(sf4e::ui::HandleOverlayMessage(window, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10), false, true, true) != 0, "chip hover leaked the mouse to the game");
    check(sf4e::ui::HandleOverlayMessage(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10), false, true, true) != 0, "chip click leaked to the game");
    check(sf4e::ui::HandleOverlayMessage(window, WM_LBUTTONUP, 0, MAKELPARAM(10, 10), false, true, true) != 0, "chip release leaked to the game");
    check(sf4e::ui::HandleOverlayMessage(window, WM_KEYDOWN, 'A', 0, false, false, true) == 0, "chip hover took the game's keys");
    SetCursor(resize);
    check(sf4e::ui::HandleOverlayMessage(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
        MAKELPARAM(HTCLIENT, WM_MOUSEMOVE), false, true, true) != 0 && GetCursor() == arrow, "chip hover did not own the cursor");
    check(sf4e::ui::HandleOverlayMessage(window, WM_MOUSEMOVE, 0, MAKELPARAM(10, 10), false, true, false) == 0, "leaving the chip kept the mouse");
    check(sf4e::ui::HandleOverlayMessage(window, WM_KEYDOWN, 'A', 0, true, true) != 0, "menu typing leaked");
    check(sf4e::ui::HandleOverlayMessage(window, WM_KEYUP, 'A', 0, false, false) == 0, "native key was swallowed");
    check(sf4e::ui::HandleOverlayMessage(window, WM_SYSKEYDOWN, VK_F10, 0, false, true) != 0, "F10 was not consumed at menu");
    check(sf4e::ui::HandleOverlayMessage(window, WM_SYSKEYDOWN, VK_F10, 0, false, false) == 0, "F10 was consumed during gameplay");
    // The click that brings the game forward is a request to focus it, not a press on the row under the pointer.
    {
        using sf4e::ui::ActivationClickFilter;
        ActivationClickFilter filter;
        check(!filter.Swallow(WM_LBUTTONDOWN, 0) && !filter.Swallow(WM_LBUTTONUP, 0), "a click with no activation was swallowed");
        check(!filter.Swallow(WM_MOUSEACTIVATE, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)), "WM_MOUSEACTIVATE itself was swallowed");
        check(!filter.Swallow(WM_MOUSEMOVE, MAKELPARAM(10, 10)) && !filter.Swallow(WM_ACTIVATEAPP, TRUE),
            "messages between the activation and its press were swallowed");
        check(filter.Swallow(WM_LBUTTONDOWN, MAKELPARAM(10, 10)) && filter.Swallow(WM_LBUTTONUP, MAKELPARAM(10, 10)),
            "the activating press or its release reached the overlay");
        check(!filter.Swallow(WM_LBUTTONDOWN, 0) && !filter.Swallow(WM_LBUTTONUP, 0), "the next click was swallowed as well");
        // Another button is not the one that activated, and a double click is the press.
        filter.Swallow(WM_MOUSEACTIVATE, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        check(!filter.Swallow(WM_RBUTTONDOWN, 0), "a different button was taken for the activating click");
        check(!filter.Swallow(WM_LBUTTONDOWN, 0) && !filter.Swallow(WM_LBUTTONUP, 0), "the activation stayed pending after another button");
        filter.Swallow(WM_MOUSEACTIVATE, MAKELPARAM(HTCLIENT, WM_RBUTTONDOWN));
        check(filter.Swallow(WM_RBUTTONDBLCLK, 0) && filter.Swallow(WM_RBUTTONUP, 0) && !filter.Swallow(WM_RBUTTONUP, 0),
            "a double click did not count as the activating press");
        // A title-bar activation or a keyboard return leaves nothing to swallow.
        filter.Swallow(WM_MOUSEACTIVATE, MAKELPARAM(HTCAPTION, WM_LBUTTONDOWN));
        check(!filter.Swallow(WM_LBUTTONDOWN, 0) && !filter.Swallow(WM_LBUTTONUP, 0), "a title-bar activation ate the next client click");
        filter.Swallow(WM_MOUSEACTIVATE, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        filter.Reset();
        check(!filter.Swallow(WM_LBUTTONDOWN, 0), "Reset kept an activation pending");
        // A pending release is dropped by Reset too.
        filter.Swallow(WM_MOUSEACTIVATE, MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        check(filter.Swallow(WM_LBUTTONDOWN, 0), "a second activation did not swallow its press");
        filter.Reset();
        check(!filter.Swallow(WM_LBUTTONUP, 0), "Reset kept a release pending");
    }
    // SF4 may deliver window messages on another thread than the one that
    // draws. With deferred input, every press and release sent from a second
    // thread reaches ImGui in order while frames run, and focus loss releases
    // what is still held.
    {
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(640, 480);
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        const auto frame = [] { ImGui_ImplWin32_NewFrame(); ImGui::NewFrame(); ImGui::EndFrame(); };
        ImGui_ImplWin32_SetDeferredInput(true);
        frame();
        std::atomic<bool> sent{false};
        int pressesSeen = 0;
        std::thread messages([&] {
            for (int i = 0; i < 2000; ++i) {
                ImGui_ImplWin32_WndProcHandler(window, WM_KEYDOWN, VK_LEFT, 0);
                ImGui_ImplWin32_WndProcHandler(window, WM_KEYUP, VK_LEFT, 0xC0000000);
                ImGui_ImplWin32_WndProcHandler(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
                ImGui_ImplWin32_WndProcHandler(window, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
                ImGui_ImplWin32_WndProcHandler(window, WM_CHAR, 'd', 0);
            }
            ImGui_ImplWin32_WndProcHandler(window, WM_KEYDOWN, VK_RIGHT, 0);
            sent = true;
        });
        while (!sent) { frame(); if (ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsMouseDown(0)) ++pressesSeen; }
        messages.join();
        // ImGui applies one transition per key each frame, so the queued
        // presses and releases take several frames to play out.
        for (int i = 0; i < 20000 && (ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsMouseDown(0) || !ImGui::IsKeyDown(ImGuiKey_RightArrow)); ++i) frame();
        frame();
        check(pressesSeen > 0, "no press from the message thread reached a frame");
        check(!ImGui::IsKeyDown(ImGuiKey_LeftArrow) && !ImGui::IsMouseDown(0), "a release from the message thread was lost");
        check(ImGui::IsKeyDown(ImGuiKey_RightArrow), "the last press from the message thread was lost");
        std::thread focus([&] { ImGui_ImplWin32_ClearInput(); });
        focus.join();
        frame();
        check(!ImGui::IsKeyDown(ImGuiKey_RightArrow), "focus loss did not release a held key");
        ImGui_ImplWin32_SetDeferredInput(false);
    }
    SetCursor(previous);
    ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); DestroyWindow(window);
    std::printf("Cursor disappeared after %d / 100 overlay mouse events\n", disappeared);
    if (!failures) std::puts("Cursor ownership, 20 visibility transitions, text cursor, native borders and keyboard routing passed");
    return failures ? 1 : 0;
}
