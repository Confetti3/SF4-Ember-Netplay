// Exercise the production overlay lifecycle without starting SF4 or networking.
#include "../sf4e/sf4e__Overlay.hxx"
#include "../sf4e/sf4e__OverlayPrefs.hxx"
#include "../ui/OverlayPresentation.hxx"
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <cstdio>

int main() {
    HWND window = CreateWindowW(L"STATIC", L"Ember display reset regression", WS_OVERLAPPEDWINDOW,
        0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 2;
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS params{};
    params.Windowed = TRUE;
    params.SwapEffect = D3DSWAPEFFECT_DISCARD;
    params.hDeviceWindow = window;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &params, &device))) {
        if (d3d) d3d->Release();
        DestroyWindow(window);
        return 2;
    }
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    namespace Overlay = sf4e::Overlay;
    Overlay::InitializeOverlay(window, device);
    Overlay::OverlayWindowFunc(window, WM_ACTIVATEAPP, TRUE, 0);
    check(Overlay::HasInputFocus(), "initial activation did not reach overlay");

    // A display transition can reactivate the window inside native Reset,
    // between FreeOverlay and InitializeOverlay, with no ImGui context alive.
    Overlay::OverlayWindowFunc(window, WM_ACTIVATEAPP, FALSE, 0);
    check(!Overlay::HasInputFocus(), "deactivation did not release focus");
    Overlay::FreeOverlay();
    check(!ImGui::GetCurrentContext(), "overlay context survived teardown");
    check(Overlay::OverlayWindowFunc(window, WM_ACTIVATEAPP, TRUE, 0) == 0,
        "activation during reset was swallowed from the native window");
    check(Overlay::HasInputFocus(), "display-reset activation was lost while ImGui was absent");
    check(SUCCEEDED(device->Reset(&params)), "hidden DX9 reset failed");
    Overlay::InitializeOverlay(window, device);
    Overlay::RequestMainControls();
    check(Overlay::CapturesMenuInput(), "overlay could not accept an open request after display reset");

    // Use the real message route and Win32 backend for the keyboard-only
    // repro, followed by the same visibility policy and F10 edge as DrawOverlay.
    const auto keyboardOpen = [&] {
        sf4e::ui::OverlayPresentation menu;
        menu.Update(true, sf4e::netplay::MatchState::None, false, Overlay::HasInputFocus());
        menu.Close();
        Overlay::OverlayWindowFunc(window, WM_SYSKEYDOWN, VK_F10, 0);
        ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        check(ImGui::IsKeyPressed(ImGuiKey_F10, false), "F10 did not reach the recreated Win32 backend");
        if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) menu.Toggle();
        check(menu.Visible(), "F10 could not reopen the safe-menu overlay after reset");
        if (menu.Visible()) {
            ImGui::SetNextWindowPos(ImVec2(10, 10));
            ImGui::SetNextWindowSize(ImVec2(320, 200));
            ImGui::Begin("Reset test"); ImGui::TextUnformatted("Overlay reopened"); ImGui::End();
        }
        ImGui::Render();
        check(ImGui::GetDrawData()->TotalVtxCount > 0, "recreated overlay produced no geometry");
        if (SUCCEEDED(device->BeginScene())) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); device->EndScene();
        } else check(false, "recreated DX9 device could not render");
        Overlay::OverlayWindowFunc(window, WM_SYSKEYUP, VK_F10, 0);
        ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame(); ImGui::EndFrame();
    };
    keyboardOpen();

    for (int reset = 0; reset < 4; ++reset) {
        Overlay::FreeOverlay();
        // Losing activation inside Reset must also survive recreation. Never
        // recover by unconditionally setting focused=true in InitializeOverlay.
        Overlay::OverlayWindowFunc(window, WM_ACTIVATEAPP, FALSE, 0);
        check(!Overlay::HasInputFocus(), "deactivation during reset was ignored");
        params.BackBufferWidth = reset % 2 ? 640 : 800;
        params.BackBufferHeight = reset % 2 ? 480 : 600;
        check(SUCCEEDED(device->Reset(&params)), "repeated hidden DX9 reset failed");
        Overlay::InitializeOverlay(window, device);
        check(!Overlay::HasInputFocus(), "recreation stole focus from another application");
        Overlay::RequestMainControls();
        check(!Overlay::CapturesMenuInput(), "unfocused overlay captured native input");
        Overlay::OverlayWindowFunc(window, WM_ACTIVATEAPP, TRUE, 0);
        keyboardOpen();
    }

    Overlay::FreeOverlay();
    sf4e::OverlayPrefs::StopPersistence();
    device->Release(); d3d->Release(); DestroyWindow(window);
    if (!failures) std::puts("Overlay activation, F10 reopening, rendering and background input across 5 real DX9 resets passed");
    return failures ? 1 : 0;
}
