#include "../sf4e/sf4e__Borderless.hxx"
#include "../common/BorderlessLayout.hxx"
#include "../platform/DisplaySettings.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "test_support.hxx"
#include <cstring>

using Graphics = Dimps::Platform::D3D;
static LRESULT CALLBACK TestWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (sf4e::display::WindowMessage(window, message, wp, lp)) return 0;
    return DefWindowProcW(window, message, wp, lp);
}
static void Pump() {
    MSG message;
    int count = 0;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        CHECK(++count < 100); // Catch window-size/reset message loops.
        DispatchMessageW(&message);
    }
}

int main(int argc, char** argv) {
    wchar_t integration[2] = {};
    if (GetEnvironmentVariableW(L"SF4E_DISPLAY_INTEGRATION_TEST", integration, 2) != 1 || integration[0] != L'1') {
        std::puts("Set SF4E_DISPLAY_INTEGRATION_TEST=1 on a Windows desktop with D3D9 to run this integration test.");
        return 77;
    }
    const bool enabled = argc == 1 || std::strcmp(argv[1], "off") != 0;
    SetEnvironmentVariableW(L"SF4E_BORDERLESS_TEST", enabled ? L"1" : L"0");
    const std::string variant = argc > 1 ? argv[1] : "";
    const bool unaware = variant == "unaware" || variant == "windowed-unaware";
    const bool windowed = variant == "windowed" || variant == "windowed-unaware";
    sf4e::display::Preferences preference;
    preference.mode = !enabled ? sf4e::display::Mode::Native : windowed ? sf4e::display::Mode::Windowed : sf4e::display::Mode::Borderless;
    if (variant == "scaled") { preference.width = 1280; preference.height = 720; }
    if (variant == "missing-monitor") preference.monitor = "disconnected-monitor";
    CHECK(sf4e::platform::SetLaunchDisplayPreferences(preference));
    const auto monitors = sf4e::platform::DisplayMonitors();
    const auto* selectedMonitor = sf4e::display::FindMonitor(monitors, preference.monitor);
    CHECK(selectedMonitor);
    const auto selection = sf4e::display::Resolve(preference, *selectedMonitor);
    SetThreadDpiAwarenessContext(unaware ? DPI_AWARENESS_CONTEXT_UNAWARE : DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW cls = {};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpfnWndProc = TestWindow;
    cls.lpszClassName = L"SF4Ember.HiddenDisplayTest";
    CHECK(RegisterClassW(&cls));
    // Remains hidden throughout the test. No game process or native config.
    HWND window = CreateWindowW(cls.lpszClassName, L"Display test", WS_OVERLAPPEDWINDOW,
        0, 0, 640, 480, nullptr, nullptr, cls.hInstance, nullptr);
    CHECK(window);
    alignas(Graphics) unsigned char memory[0x300] = {};
    Graphics* graphics = reinterpret_cast<Graphics*>(memory);
    graphics->hFocusWindow = window;
    graphics->deviceType = D3DDEVTYPE_HAL;
    *Graphics::GetConfiguredWidth(graphics) = 1920;
    *Graphics::GetConfiguredHeight(graphics) = 1080;
    *Graphics::GetConfiguredFullscreen(graphics) = 1;
    *Graphics::GetConfiguredRefresh(graphics) = 60.f;
    sf4e::display::Prepare(graphics); // No D3D instance: must leave settings alone.
    CHECK(*Graphics::GetConfiguredFullscreen(graphics) == 1);
    CHECK(*Graphics::GetConfiguredWidth(graphics) == 1920);
    graphics->lpD3D = Direct3DCreate9(D3D_SDK_VERSION);
    CHECK(graphics->lpD3D);
    sf4e::display::Prepare(graphics);
    sf4e::display::RequestApply(window);
    Pump();
    // Inspect physical output even when the original game-style window was
    // created without per-monitor DPI awareness.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (enabled) {
        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        CHECK(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor));
        const auto layout = sf4e::display::Fit16By9(monitor.rcMonitor.right - monitor.rcMonitor.left,
            monitor.rcMonitor.bottom - monitor.rcMonitor.top);
        RECT client = {}, outer = {};
        CHECK(GetClientRect(window, &client) && GetWindowRect(window, &outer));
        CHECK(client.right == selection.window.width && client.bottom == selection.window.height);
        POINT origin = {0, 0}; CHECK(ClientToScreen(window, &origin));
        CHECK(origin.x == selection.window.x && origin.y == selection.window.y);
        POINT center = {client.right / 2, client.bottom / 2};
        CHECK(ClientToScreen(window, &center));
        CHECK(center.x == origin.x + client.right / 2 && center.y == origin.y + client.bottom / 2);
        CHECK(*Graphics::GetConfiguredWidth(graphics) == static_cast<DWORD>(selection.width));
        CHECK(*Graphics::GetConfiguredHeight(graphics) == static_cast<DWORD>(selection.height));
        CHECK(*Graphics::GetConfiguredFullscreen(graphics) == 0);
        CHECK(((GetWindowLongW(window, GWL_STYLE) & WS_CAPTION) != 0) == windowed);
        CHECK(!IsWindowVisible(window));
        HWND bars = FindWindowW(L"SF4Ember.BorderlessBars.v1", nullptr);
        if (!windowed && (layout.x || layout.y)) {
            CHECK(bars && GetWindow(bars, GW_OWNER) == window && !IsWindowVisible(bars));
            HRGN region = CreateRectRgn(0, 0, 0, 0);
            CHECK(GetWindowRgn(bars, region) != ERROR);
            CHECK(!PtInRegion(region, layout.x + layout.width / 2, layout.y + layout.height / 2));
            CHECK(PtInRegion(region, 0, 0));
            DeleteObject(region);
        }
        if (windowed) CHECK(!bars);
        CHECK(sf4e::display::WindowMessage(window, WM_SYSKEYDOWN, VK_RETURN, 1L << 29));
        CHECK(!sf4e::display::WindowMessage(window, WM_SYSKEYDOWN, VK_F4, 1L << 29));
        D3DPRESENT_PARAMETERS present = {};
        present.Windowed = TRUE;
        present.hDeviceWindow = window;
        present.BackBufferWidth = selection.width;
        present.BackBufferHeight = selection.height;
        present.SwapEffect = D3DSWAPEFFECT_DISCARD;
        present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9* device = nullptr;
        CHECK(SUCCEEDED(graphics->lpD3D->CreateDevice(0, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present, &device)));
        CHECK(SUCCEEDED(device->Reset(&present)));
        IDirect3DSurface9* buffer = nullptr;
        CHECK(SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &buffer)));
        D3DSURFACE_DESC description = {};
        CHECK(SUCCEEDED(buffer->GetDesc(&description)));
        CHECK(description.Width == static_cast<UINT>(selection.width) && description.Height == static_cast<UINT>(selection.height));
        buffer->Release();
        device->Release();
        sf4e::display::Prepare(graphics);
        sf4e::display::RequestApply(window);
        Pump();
        sf4e::display::WindowMessage(window, WM_ACTIVATEAPP, FALSE, 0);
        CHECK(!(GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOPMOST));
        CHECK(GetClientRect(window, &client) && client.right == selection.window.width && client.bottom == selection.window.height);
        CHECK(DestroyWindow(window));
        CHECK(!bars || !IsWindow(bars));
        std::printf("Borderless Win32/D3D9: monitor %ldx%ld, render %dx%d, offset %d,%d; hidden window/create/reset/cleanup passed\n",
            monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
            selection.width, selection.height, selection.window.x, selection.window.y);
    } else {
        CHECK(*Graphics::GetConfiguredWidth(graphics) == 1920);
        CHECK(*Graphics::GetConfiguredFullscreen(graphics) == 1);
        CHECK(GetWindowLongW(window, GWL_STYLE) & WS_CAPTION);
        CHECK(!FindWindowW(L"SF4Ember.BorderlessBars.v1", nullptr));
        CHECK(!sf4e::display::WindowMessage(window, WM_SYSKEYDOWN, VK_RETURN, 1L << 29));
        CHECK(DestroyWindow(window));
    }
    sf4e::display::RestoreNative(graphics);
    sf4e::display::RestoreNative(); // Repeated cleanup is harmless.
    CHECK(*Graphics::GetConfiguredWidth(graphics) == 1920 && *Graphics::GetConfiguredHeight(graphics) == 1080);
    CHECK(*Graphics::GetConfiguredFullscreen(graphics) == 1 && *Graphics::GetConfiguredRefresh(graphics) == 60.f);
    graphics->lpD3D->Release();
    return 0;
}
