#include "sf4e__Borderless.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../common/BorderlessLayout.hxx"
#include "../common/EnvFlag.hxx"
#include "../platform/DisplaySettings.hxx"
#include <atomic>
#include "spdlog/spdlog.h"

namespace sf4e { namespace display {
namespace {
const Preferences& Requested() {
    static const auto preferences = [] {
        std::string error;
        const auto value = platform::LaunchDisplayPreferences(error);
        if (!error.empty()) spdlog::warn("Display: {}", error);
        return value;
    }();
    return preferences;
}
bool Enabled() { return Requested().mode != Mode::Native; }
UINT ApplyMessage() {
    static const UINT message = RegisterWindowMessageW(L"SF4Ember.BorderlessTest.Apply.v1");
    return message;
}
// The graphics thread publishes an immutable layout before any window-thread
// work is queued. HWND/bar visibility state below belongs only to that thread.
struct Plan {
    RECT monitor = {};
    Layout area;
    Selection selection;
    unsigned adapter = 0;
};
Plan plan;
std::atomic<bool> prepared{false};
HWND bars = nullptr;
bool applying = false;
bool applied = false;
struct NativeValues {
    Dimps::Platform::D3D* graphics = nullptr;
    DWORD width = 0, height = 0, fullscreen = 0, adapter = 0;
    float refresh = 0;
} native;

struct PhysicalPixels {
    DPI_AWARENESS_CONTEXT previous;
    PhysicalPixels() : previous(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalPixels() { if (previous) SetThreadDpiAwarenessContext(previous); }
};

LRESULT CALLBACK BarsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(window, message, wParam, lParam);
}

bool CreateBars(HWND owner) {
    const int width = plan.monitor.right - plan.monitor.left;
    const int height = plan.monitor.bottom - plan.monitor.top;
    if (width == plan.area.width && height == plan.area.height) return true;
    WNDCLASSW cls = {};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"SF4Ember.BorderlessBars.v1";
    cls.lpfnWndProc = BarsProc;
    cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    bars = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName,
        L"", WS_POPUP, plan.monitor.left, plan.monitor.top, width, height,
        owner, nullptr, cls.hInstance, nullptr);
    if (!bars) return false;
    // An owned window stays above its owner. Cut out the entire game rectangle
    // so the bars can never cover the renderer or intercept game input.
    HRGN region = CreateRectRgn(0, 0, width, height);
    HRGN hole = CreateRectRgn(plan.area.x, plan.area.y,
        plan.area.x + plan.area.width, plan.area.y + plan.area.height);
    const bool ok = region && hole && CombineRgn(region, region, hole, RGN_DIFF) != ERROR
        && SetWindowRgn(bars, region, TRUE);
    if (hole) DeleteObject(hole);
    if (!ok) {
        if (region) DeleteObject(region); // On success Windows owns this region.
        DestroyWindow(bars);
        bars = nullptr;
    }
    return ok;
}

bool SetStyle(HWND window, int index, LONG_PTR value) {
    SetLastError(ERROR_SUCCESS);
    return SetWindowLongPtrW(window, index, value) != 0 || GetLastError() == ERROR_SUCCESS;
}

void FocusChanged(HWND window, bool active) {
    // The 16:9 window does not cover the whole ultrawide desktop, so the shell
    // need not classify it as fullscreen. Cover the taskbar only while active;
    // owned bars follow the owner's z-order and never take focus themselves.
    SetWindowPos(window, active && plan.selection.mode == Mode::Borderless ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (bars) ShowWindow(bars, active && !IsIconic(window) ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void Apply(HWND window) {
    if (applying || IsIconic(window)) return;
    if (plan.selection.mode == Mode::Fullscreen) {
        if (!applied) spdlog::info("Display: exclusive fullscreen adapter={} render={}x{} refresh={}Hz fallback={}",
            plan.adapter, plan.selection.width, plan.selection.height, plan.selection.refresh, plan.selection.fallback);
        applied = true;
        return; // D3D owns fullscreen window sizing and the display mode.
    }
    PhysicalPixels pixels;
    applying = true;
    const LONG_PTR oldStyle = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR oldExStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const bool windowed = plan.selection.mode == Mode::Windowed;
    const LONG_PTR style = windowed
        ? (oldStyle & ~static_cast<LONG_PTR>(WS_POPUP | WS_OVERLAPPEDWINDOW)) | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
        : (oldStyle & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_POPUP;
    const LONG_PTR exStyle = oldExStyle & ~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
    RECT existing = {};
    GetWindowRect(window, &existing);
    RECT desired = {plan.selection.window.x, plan.selection.window.y,
        plan.selection.window.x + plan.selection.window.width, plan.selection.window.y + plan.selection.window.height};
    if (windowed) AdjustWindowRectExForDpi(&desired, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle), GetDpiForWindow(window));
    if (applied && style == oldStyle && exStyle == oldExStyle
        && (windowed || (existing.left == desired.left && existing.top == desired.top))
        && existing.right - existing.left == desired.right - desired.left && existing.bottom - existing.top == desired.bottom - desired.top) {
        FocusChanged(window, GetForegroundWindow() == window);
        applying = false;
        return; // In particular, do not generate another WM_SIZE/Reset cycle.
    }
    const bool ok = SetStyle(window, GWL_STYLE, style) && SetStyle(window, GWL_EXSTYLE, exStyle)
        && SetWindowPos(window, nullptr, desired.left, desired.top,
            desired.right - desired.left, desired.bottom - desired.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
    if (!ok) {
        const DWORD error = GetLastError();
        SetStyle(window, GWL_STYLE, oldStyle);
        SetStyle(window, GWL_EXSTYLE, oldExStyle);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
        spdlog::error("Display: could not apply window layout (Win32 {}); previous window style retained", error);
    } else {
        if (!applied) {
            if (!windowed && !CreateBars(window)) spdlog::warn("Display: black bars could not be created (Win32 {})", GetLastError());
            spdlog::info("Display: mode={} monitor={}x{} origin={},{} render={}x{} client={}x{} offset={},{} fallback={}; fixed 16:9",
                windowed ? "windowed" : "borderless",
                plan.monitor.right - plan.monitor.left, plan.monitor.bottom - plan.monitor.top,
                plan.monitor.left, plan.monitor.top, plan.selection.width, plan.selection.height,
                plan.selection.window.width, plan.selection.window.height, plan.area.x, plan.area.y, plan.selection.fallback);
        }
        applied = true;
        FocusChanged(window, GetForegroundWindow() == window);
    }
    applying = false;
}
}

void Prepare(Dimps::Platform::D3D* graphics) {
    if (!Enabled()) return;
    if (!prepared.load(std::memory_order_acquire)) {
        PhysicalPixels pixels;
        const auto monitors = platform::DisplayMonitors();
        const auto* monitor = FindMonitor(monitors, Requested().monitor);
        if (!monitor) return;
        const Layout area = Fit16By9(monitor->width, monitor->height);
        const auto selection = Resolve(Requested(), *monitor);
        D3DCAPS9 caps = {};
        if (!area.Valid() || !selection.window.Valid() || !graphics->lpD3D || FAILED(graphics->lpD3D->GetDeviceCaps(monitor->adapter,
            static_cast<D3DDEVTYPE>(graphics->deviceType), &caps))
            || static_cast<UINT>(selection.width) > caps.MaxTextureWidth
            || static_cast<UINT>(selection.height) > caps.MaxTextureHeight) return;
        plan.monitor = {monitor->x, monitor->y, monitor->x + monitor->width, monitor->y + monitor->height};
        plan.area = area;
        plan.selection = selection;
        plan.adapter = monitor->adapter;
        prepared.store(true, std::memory_order_release);
    }
    // Verified in the supported executable's BuildPresentParameters (0x7719d0).
    // Configure the engine's own size as well as the D3D backbuffer; merely
    // stretching the native window would leave a lower-resolution image.
    if (!native.graphics) {
        native.graphics = graphics;
        native.width = *Dimps::Platform::D3D::GetConfiguredWidth(graphics);
        native.height = *Dimps::Platform::D3D::GetConfiguredHeight(graphics);
        native.fullscreen = *Dimps::Platform::D3D::GetConfiguredFullscreen(graphics);
        native.refresh = *Dimps::Platform::D3D::GetConfiguredRefresh(graphics);
        native.adapter = graphics->workingMem;
    }
    graphics->workingMem = plan.adapter;
    *Dimps::Platform::D3D::GetConfiguredWidth(graphics) = plan.selection.width;
    *Dimps::Platform::D3D::GetConfiguredHeight(graphics) = plan.selection.height;
    *Dimps::Platform::D3D::GetConfiguredFullscreen(graphics) = plan.selection.mode == Mode::Fullscreen ? 1 : 0;
    if (plan.selection.refresh) *Dimps::Platform::D3D::GetConfiguredRefresh(graphics) = static_cast<float>(plan.selection.refresh);
}

void RestoreNative(Dimps::Platform::D3D* graphics) {
    if (!native.graphics || (graphics && graphics != native.graphics)) return;
    // The game may persist its display values during shutdown. Hand it back
    // the original values rather than making a launcher override its new
    // native default. No config.ini edit is made by Ember.
    auto* original = native.graphics;
    original->workingMem = native.adapter;
    *Dimps::Platform::D3D::GetConfiguredWidth(original) = native.width;
    *Dimps::Platform::D3D::GetConfiguredHeight(original) = native.height;
    *Dimps::Platform::D3D::GetConfiguredFullscreen(original) = native.fullscreen;
    *Dimps::Platform::D3D::GetConfiguredRefresh(original) = native.refresh;
    native.graphics = nullptr;
}

void RequestApply(HWND window) {
    if (!Enabled()) return;
    if (!prepared.load(std::memory_order_acquire)) {
        spdlog::warn("Display: layout/device capability check failed; native display settings retained");
        return;
    }
    const UINT message = ApplyMessage();
    if (!message || !PostMessageW(window, message, 0, 0))
        spdlog::error("Display: could not queue window setup (Win32 {})", GetLastError());
}

bool WindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (!Enabled() || !prepared.load(std::memory_order_acquire)) return false;
    if (ApplyMessage() && message == ApplyMessage()) {
        Apply(window);
        return true;
    }
    if (message == WM_ACTIVATEAPP && applied && plan.selection.mode != Mode::Fullscreen) FocusChanged(window, wParam != 0);
    if (message == WM_SIZE && wParam == SIZE_MINIMIZED && bars) ShowWindow(bars, SW_HIDE);
    if (message == WM_SIZE && wParam == SIZE_RESTORED && applied && !applying) RequestApply(window);
    // The selected mode is fixed for this process. Do not let native Alt+Enter
    // switch it behind the layout. Alt+F4 remains available.
    if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && wParam == VK_RETURN && (lParam & (1L << 29))) return true;
    if (message == WM_DESTROY) {
        if (bars) DestroyWindow(bars);
        bars = nullptr;
        applied = false;
    }
    return false;
}
} }
