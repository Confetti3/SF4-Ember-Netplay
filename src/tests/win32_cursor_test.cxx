// Drive the real Win32 backend and overlay message route on a hidden window.
// A game-style fallback clears the OS cursor if overlay handling falls through.
#include "../ui/Win32Input.hxx"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    // draws. Through the input bridge, every press and release sent from a
    // second thread reaches ImGui in order while frames run, and focus loss
    // releases what is still held.
    {
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(640, 480);
        unsigned char* pixels = nullptr; int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        const auto frame = [] { ImGui_ImplWin32_NewFrame(); ImGui::NewFrame(); ImGui::EndFrame(); };
        sf4e::ui::Win32InputBridge bridge;
        ImGui_ImplWin32_SetInputBridge(&bridge);
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
        std::thread focus([&] { bridge.RequestClear(); });
        focus.join();
        frame();
        check(!ImGui::IsKeyDown(ImGuiKey_RightArrow), "focus loss did not release a held key");

        // A drawing thread that stops taking input cannot grow the queue without
        // end, and a release the bound drops still leaves no key held.
        using sf4e::ui::Win32Input;
        const auto overflowsBefore = bridge.Overflows();
        bridge.Push(Win32Input::KeyEvent(ImGuiKey_LeftArrow, true, VK_LEFT));
        frame();
        check(ImGui::IsKeyDown(ImGuiKey_LeftArrow), "a queued press did not apply");
        bridge.Push(Win32Input::KeyEvent(ImGuiKey_LeftArrow, false, VK_LEFT));
        for (std::size_t i = 0; i < sf4e::ui::Win32InputBridge::MaxQueuedEvents; ++i)
            bridge.Push(Win32Input::Of(Win32Input::MousePos, 0, 0, static_cast<float>(i % 100), 5.f));
        check(bridge.Overflows() == overflowsBefore + 1, "the input bound did not trip");
        frame();
        check(!ImGui::IsKeyDown(ImGuiKey_LeftArrow), "a release dropped at the bound left the key held");

        // Go now's Enter on the Training call, routed as the overlay's window
        // procedure routes it: the go now key first, then the overlay's
        // handling into the bridge, and what neither keeps goes to the game.
        // At the call's onset, under the game's pause and at the call's end,
        // a press is either the call's alone (a request for that call, and
        // neither the overlay nor the game sees it) or not the call's at all.
        {
            sf4e::input::GoNowGate gate;
            sf4e::ui::GoNowKey key;
            int gameDowns = 0;
            const auto route = [&](UINT message, WPARAM w, LPARAM l, bool free) {
                if (key.Take(message, w, l, gate, free)) return;
                if (!sf4e::ui::HandleOverlayMessage(window, message, w, l, false, false) && message == WM_KEYDOWN && w == VK_RETURN) ++gameDowns;
            };
            const auto enter = [&](bool free, bool repeat = false) {
                route(WM_KEYDOWN, VK_RETURN, repeat ? (1L << 30) : 0, free);
                route(WM_CHAR, L'\r', 0, free);
            };
            const auto release = [&](bool free) { route(WM_KEYUP, VK_RETURN, 0xC0000000, free); };
            const auto overlaySawEnter = [&] { frame(); const bool seen = ImGui::IsKeyPressed(ImGuiKey_Enter, false); frame(); return seen; };
            sf4e::input::CallIdentity call; call.roomEpoch = 3; call.table = 0; call.opponent = 2; call.generation = 9;
            // Before the call: Enter is the game's and the overlay's, and asks for nothing.
            enter(true);
            check(gameDowns == 1 && gate.Take().empty(), "Enter before the call was kept or went now");
            check(overlaySawEnter(), "Enter before the call did not reach the overlay");
            release(true); frame();
            // The call's onset: the next fresh Enter is the call's alone, its
            // repeats and character too, and its release goes on.
            gate.Offer(call);
            enter(true); enter(true, true); enter(true, true);
            auto requests = gate.Take();
            check(gameDowns == 1 && requests.size() == 1 && requests[0].call == call, "Enter on the call reached the game or did not go now once");
            check(!overlaySawEnter(), "Enter taken for go now reached the overlay");
            release(true); frame();
            // The game's pause opens: Enter is its menu's, and goes nowhere else.
            enter(false); enter(false, true);
            check(gameDowns == 3 && gate.Take().empty(), "Enter under the pause menu went now or was kept from it");
            release(false); frame();
            // An Enter held from before the call is not go now when it repeats.
            gate.Offer(sf4e::input::CallIdentity{});
            enter(true); gate.Offer(call); enter(true, true);
            check(gameDowns == 5 && gate.Take().empty(), "A repeat of an Enter held before the call went now");
            release(true); frame();
            // The call ends: Enter is the game's again; a request taken just
            // before the end names the call that has gone.
            enter(true); release(true);
            requests = gate.Take();
            sf4e::input::CallIdentity next = call; next.opponent = 0; gate.Offer(next);
            check(requests.size() == 1 && requests[0].call == call && requests[0].call != gate.Offered(), "A request outlived its call unmarked");
            enter(true);
            check(gameDowns == 6 && gate.Take().empty(), "Enter after the call ended went now");
            release(true); frame(); frame();
            // The call's lifecycle sets the offer. The player leaves Training
            // for character select while the opponent stays seated: the call
            // still stands (that is not the main menu), but its battle has
            // closed, so Enter is the game's again; so it is in a later
            // Training battle, which the call was not sent to.
            sf4e::input::CallLifecycle lifecycle;
            call.serial = 5;
            lifecycle.Tick(call, sf4e::input::CallBattle{true, call.generation}, gate);
            enter(true); release(true);
            check(gameDowns == 6 && gate.Offered() == call, "Enter was not go now while the call's battle ran");
            lifecycle.Tick(call, sf4e::input::CallBattle{false, call.generation}, gate);
            check(lifecycle.Dropped() == 1, "A press taken just before the battle closed went now for it");
            enter(true); release(true);
            check(gameDowns == 7 && gate.Take().empty(), "Enter after the call's battle closed was kept from the game");
            check(overlaySawEnter(), "Enter after the call's battle closed did not reach the overlay");
            lifecycle.Tick(call, sf4e::input::CallBattle{true, call.generation + 1}, gate);
            enter(true); release(true);
            check(gameDowns == 8 && gate.Take().empty(), "Enter in a later Training battle went now for the old call");
            frame(); frame();
        }

        // A real window procedure on the message thread: releasing the mouse
        // makes Windows send WM_CAPTURECHANGED back into it, and so into the
        // backend, from inside the backend's own ReleaseCapture call. That
        // must finish while the drawing thread keeps running frames.
        static std::atomic<int> captureChanges{0};
        struct Owner {
            static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
                if (message == WM_CAPTURECHANGED) ++captureChanges;
                ImGui_ImplWin32_WndProcHandler(hwnd, message, w, l);
                return DefWindowProcW(hwnd, message, w, l);
            }
        };
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = Owner::Proc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"EmberCaptureRegression";
        RegisterClassW(&windowClass);
        std::atomic<bool> captured{false}, finished{false};
        std::thread owner([&] {
            HWND own = CreateWindowW(windowClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                nullptr, nullptr, windowClass.hInstance, nullptr);
            for (int press = 0; own && press < 50; ++press) {
                SendMessageW(own, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
                if (GetCapture() == own) captured = true;
                SendMessageW(own, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
            }
            if (own) DestroyWindow(own);
            finished = true;
        });
        const auto started = GetTickCount64();
        while (!finished && GetTickCount64() - started < 5000) frame();
        if (!finished) {
            std::printf("FAIL: releasing the mouse inside a real window procedure did not return\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
        owner.join();
        check(captured, "the test window never held the capture, so its release was not exercised");
        check(captureChanges >= 50, "releasing the capture did not re-enter the window procedure");
        for (int i = 0; i < 1000 && ImGui::IsMouseDown(0); ++i) frame();
        check(!ImGui::IsMouseDown(0), "a release from the capturing window was lost");

        // The game can draw at another size than its window's client area (a window the
        // screen clips, a borderless tool, a 16:10 desktop). With that render size ImGui
        // lays out in the game's pixels, and the mouse lands on what is drawn under it.
        RECT client{}; GetClientRect(window, &client);
        const float renderW = 1920, renderH = 1080;
        ImGui_ImplWin32_SetRenderSize(renderW, renderH);
        ImGui_ImplWin32_WndProcHandler(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 50));
        frame();
        check(io.DisplaySize.x == renderW && io.DisplaySize.y == renderH, "the render size did not become the display size");
        const float wantX = 100 * renderW / client.right, wantY = 50 * renderH / client.bottom;
        check(std::fabs(io.MousePos.x - wantX) < 1 && std::fabs(io.MousePos.y - wantY) < 1, "the mouse was not scaled to the render size");
        ImGui_ImplWin32_SetRenderSize(0, 0);
        ImGui_ImplWin32_WndProcHandler(window, WM_MOUSEMOVE, 0, MAKELPARAM(100, 50));
        frame();
        check(io.DisplaySize.x == client.right && io.DisplaySize.y == client.bottom, "without a render size the display is not the client area");
        check(io.MousePos.x == 100 && io.MousePos.y == 50, "without a render size the mouse was scaled");
        ImGui_ImplWin32_SetInputBridge(nullptr);
    }
    SetCursor(previous);
    ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); DestroyWindow(window);
    std::printf("Cursor disappeared after %d / 100 overlay mouse events\n", disappeared);
    if (!failures) std::puts("Cursor ownership, 20 visibility transitions, text cursor, native borders and keyboard routing passed");
    return failures ? 1 : 0;
}
