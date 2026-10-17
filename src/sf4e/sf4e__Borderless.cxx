#include "sf4e__Borderless.hxx"
#include "../Dimps/Dimps__Platform.hxx"
#include "../common/BorderlessLayout.hxx"
#include "../common/EnvFlag.hxx"
#include "../platform/DisplaySettings.hxx"
#include <atomic>
#include <mutex>
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
struct OverrideState {
    Plan plan;
    std::atomic<bool> prepared{false},cancelled{false};
    // Device fields are touched only by the graphics hooks. The config writer
    // reads an immutable original snapshot under this mutex. Window failures
    // publish cancellation; the next device hook restores before native reset.
    struct Native {
        Dimps::Platform::D3D* graphics=nullptr;
        DWORD width=0,height=0,fullscreen=0,adapter=0; float refresh=0;
        bool captured=false;
    } native;
    std::mutex deviceMutex;
    // Window-thread-only state, after the immutable plan was published.
    HWND bars=nullptr;
    bool applying=false,applied=false;
#ifdef SF4E_DISPLAY_TESTING
    WindowFault fault=WindowFault::None;
#endif
} state;
void Cancel(const char* operation,DWORD error) {
    state.cancelled.store(true,std::memory_order_release);
    spdlog::error("Display: {} failed (Win32 {}); override cancelled, native window controls restored",operation,error);
}

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
    const int width = state.plan.monitor.right - state.plan.monitor.left;
    const int height = state.plan.monitor.bottom - state.plan.monitor.top;
    if (width == state.plan.area.width && height == state.plan.area.height) return true;
    WNDCLASSW cls = {};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"SF4Ember.BorderlessBars.v1";
    cls.lpfnWndProc = BarsProc;
    cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    state.bars = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName,
        L"", WS_POPUP, state.plan.monitor.left, state.plan.monitor.top, width, height,
        owner, nullptr, cls.hInstance, nullptr);
    if (!state.bars) return false;
    // An owned window stays above its owner. Cut out the entire game rectangle
    // so the bars can never cover the renderer or intercept game input.
    HRGN region = CreateRectRgn(0, 0, width, height);
    HRGN hole = CreateRectRgn(state.plan.area.x, state.plan.area.y,
        state.plan.area.x + state.plan.area.width, state.plan.area.y + state.plan.area.height);
    const bool ok = region && hole && CombineRgn(region, region, hole, RGN_DIFF) != ERROR
        && SetWindowRgn(state.bars, region, TRUE);
    if (hole) DeleteObject(hole);
    if (!ok) {
        if (region) DeleteObject(region); // On success Windows owns this region.
        DestroyWindow(state.bars);
        state.bars = nullptr;
    }
    return ok;
}

bool SetStyle(HWND window, int index, LONG_PTR value) {
#ifdef SF4E_DISPLAY_TESTING
    if(state.fault==WindowFault::Style){state.fault=WindowFault::None;SetLastError(ERROR_ACCESS_DENIED);return false;}
#endif
    SetLastError(ERROR_SUCCESS);
    return SetWindowLongPtrW(window, index, value) != 0 || GetLastError() == ERROR_SUCCESS;
}

bool Position(HWND window,int x,int y,int width,int height) {
#ifdef SF4E_DISPLAY_TESTING
    if(state.fault==WindowFault::Position){state.fault=WindowFault::None;SetLastError(ERROR_ACCESS_DENIED);return false;}
#endif
    return SetWindowPos(window,nullptr,x,y,width,height,SWP_NOACTIVATE|SWP_NOZORDER|SWP_FRAMECHANGED)!=FALSE;
}
// Query a temporary, hidden PM-aware window on the destination monitor. Unlike
// GetDpiForWindow(game), this is neither the old monitor's DPI nor always 96
// for a legacy unaware game. It never changes the game's awareness.
UINT DestinationDpi(const RECT& monitor) {
    HWND probe=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP,
        monitor.left+32,monitor.top+32,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!probe)return 0;
    const UINT dpi=GetDpiForWindow(probe);DestroyWindow(probe);return dpi;
}
bool ClientBounds(HWND window,RECT& bounds) {
    RECT client{};
    if(!GetClientRect(window,&client))return false;
    POINT first{client.left,client.top},last{client.right,client.bottom};
    if(!ClientToScreen(window,&first)||!ClientToScreen(window,&last))return false;
    bounds={first.x,first.y,last.x,last.y};return true;
}
bool SameClient(const RECT& got,const Layout& wanted,bool position) {
    return (!position||(got.left==wanted.x&&got.top==wanted.y))&&
        got.right-got.left==wanted.width&&got.bottom-got.top==wanted.height;
}
bool WindowedFrame(HWND window,DWORD style,DWORD exStyle,RECT& desired) {
    const UINT destination=DestinationDpi(state.plan.monitor);
    if(!destination)return false;
    const auto awareness=GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(window));
    const UINT logical=awareness==DPI_AWARENESS_UNAWARE?96:
        awareness==DPI_AWARENESS_SYSTEM_AWARE?GetDpiForWindow(window):destination;
    if(!logical)return false;
    RECT frame{};
    if(!AdjustWindowRectExForDpi(&frame,style,FALSE,exStyle,logical))return false;
    // Unaware/system-aware non-client metrics are scaled by Windows to the
    // destination; a PM-aware window already uses that monitor's metrics.
    desired.left+=MulDiv(frame.left,destination,logical);
    desired.top+=MulDiv(frame.top,destination,logical);
    desired.right+=MulDiv(frame.right,destination,logical);
    desired.bottom+=MulDiv(frame.bottom,destination,logical);
    return true;
}
void FocusChanged(HWND window, bool active) {
    // The 16:9 window does not cover the whole ultrawide desktop, so the shell
    // need not classify it as fullscreen. Cover the taskbar only while active;
    // owned bars follow the owner's z-order and never take focus themselves.
    SetWindowPos(window, active && state.plan.selection.mode == Mode::Borderless ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (state.bars) ShowWindow(state.bars, active && !IsIconic(window) ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void Apply(HWND window) {
    if (state.cancelled.load(std::memory_order_acquire)||state.applying || IsIconic(window)) return;
    if (state.plan.selection.mode == Mode::Fullscreen) {
        if (!state.applied) spdlog::info("Display: exclusive fullscreen adapter={} render={}x{} refresh={}Hz fallback={}",
            state.plan.adapter, state.plan.selection.width, state.plan.selection.height, state.plan.selection.refresh, state.plan.selection.fallback);
        state.applied = true;
        return; // D3D owns fullscreen window sizing and the display mode.
    }
    PhysicalPixels pixels;
    state.applying = true;
    const LONG_PTR oldStyle = GetWindowLongPtrW(window, GWL_STYLE);
    const LONG_PTR oldExStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const bool windowed = state.plan.selection.mode == Mode::Windowed;
    const LONG_PTR style = windowed
        ? (oldStyle & ~static_cast<LONG_PTR>(WS_POPUP | WS_OVERLAPPEDWINDOW)) | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX
        : (oldStyle & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_POPUP;
    const LONG_PTR exStyle = oldExStyle & ~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
    RECT existing={},client={};
    if(!GetWindowRect(window,&existing)||!ClientBounds(window,client)){
        Cancel("read window geometry",GetLastError());state.applying=false;return;
    }
    const auto& wanted=state.plan.selection.window;
    RECT desired={wanted.x,wanted.y,wanted.x+wanted.width,wanted.y+wanted.height};
    if(windowed&&!WindowedFrame(window,static_cast<DWORD>(style),static_cast<DWORD>(exStyle),desired)){
        Cancel("calculate destination frame",GetLastError());state.applying=false;return;
    }
    if(state.applied&&style==oldStyle&&exStyle==oldExStyle&&SameClient(client,wanted,!windowed)){
        FocusChanged(window,GetForegroundWindow()==window);state.applying=false;return;
    }
    bool ok=SetStyle(window,GWL_STYLE,style)&&SetStyle(window,GWL_EXSTYLE,exStyle)&&
        Position(window,desired.left,desired.top,desired.right-desired.left,desired.bottom-desired.top);
    // Windows can recompute the non-client frame during WM_DPICHANGED. Verify
    // physical client geometry after the move and correct a bounded number of
    // times, rather than accepting an outer rectangle with the wrong client.
    for(int attempt=0;ok&&attempt<3;++attempt){
        ok=ClientBounds(window,client);
        if(!ok||SameClient(client,wanted,true))break;
        RECT outer{};ok=GetWindowRect(window,&outer)!=FALSE;
        if(ok)ok=Position(window,outer.left+wanted.x-client.left,outer.top+wanted.y-client.top,
            outer.right-outer.left+wanted.width-(client.right-client.left),
            outer.bottom-outer.top+wanted.height-(client.bottom-client.top));
    }
    if(ok)ok=ClientBounds(window,client)&&SameClient(client,wanted,true);
    if (!ok) {
        const DWORD error = GetLastError();
        SetStyle(window, GWL_STYLE, oldStyle);
        SetStyle(window, GWL_EXSTYLE, oldExStyle);
        SetWindowPos(window,nullptr,existing.left,existing.top,existing.right-existing.left,existing.bottom-existing.top,
            SWP_NOACTIVATE|SWP_NOZORDER|SWP_FRAMECHANGED);
        if(state.bars){DestroyWindow(state.bars);state.bars=nullptr;}
        state.applied=false;
        Cancel("apply window layout",error?error:ERROR_INVALID_DATA);
    } else {
        if (!state.applied) {
            if (!windowed && !CreateBars(window)) spdlog::warn("Display: black bars could not be created (Win32 {})", GetLastError());
            spdlog::info("Display: mode={} monitor={}x{} origin={},{} render={}x{} client={}x{} offset={},{} fallback={}; fixed 16:9",
                windowed ? "windowed" : "borderless",
                state.plan.monitor.right - state.plan.monitor.left, state.plan.monitor.bottom - state.plan.monitor.top,
                state.plan.monitor.left, state.plan.monitor.top, state.plan.selection.width, state.plan.selection.height,
                state.plan.selection.window.width, state.plan.selection.window.height, state.plan.area.x, state.plan.area.y, state.plan.selection.fallback);
        }
        state.applied = true;
        FocusChanged(window, GetForegroundWindow() == window);
    }
    state.applying = false;
}
}

void Prepare(Dimps::Platform::D3D* graphics) {
    if (!Enabled()) return;
    if(state.cancelled.load(std::memory_order_acquire)){RestoreNative(graphics);return;}
    std::lock_guard<std::mutex> lock(state.deviceMutex);
    if (!state.prepared.load(std::memory_order_acquire)) {
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
        state.plan.monitor = {monitor->x, monitor->y, monitor->x + monitor->width, monitor->y + monitor->height};
        state.plan.area = area;
        state.plan.selection = selection;
        state.plan.adapter = monitor->adapter;
    }
    // Verified in the supported executable's BuildPresentParameters (0x7719d0).
    // Configure the engine's own size as well as the D3D backbuffer; merely
    // stretching the native window would leave a lower-resolution image.
    if (!state.native.graphics) {
        state.native.graphics = graphics;
        state.native.captured = true;
        state.native.width = *Dimps::Platform::D3D::GetConfiguredWidth(graphics);
        state.native.height = *Dimps::Platform::D3D::GetConfiguredHeight(graphics);
        state.native.fullscreen = *Dimps::Platform::D3D::GetConfiguredFullscreen(graphics);
        state.native.refresh = *Dimps::Platform::D3D::GetConfiguredRefresh(graphics);
        state.native.adapter = graphics->adapterOrdinal;
    }
    graphics->adapterOrdinal = state.plan.adapter;
    *Dimps::Platform::D3D::GetConfiguredWidth(graphics) = state.plan.selection.width;
    *Dimps::Platform::D3D::GetConfiguredHeight(graphics) = state.plan.selection.height;
    *Dimps::Platform::D3D::GetConfiguredFullscreen(graphics) = state.plan.selection.mode == Mode::Fullscreen ? 1 : 0;
    if (state.plan.selection.refresh) *Dimps::Platform::D3D::GetConfiguredRefresh(graphics) = static_cast<float>(state.plan.selection.refresh);
    state.prepared.store(true,std::memory_order_release);
}

void RestoreNative(Dimps::Platform::D3D* graphics) {
    std::lock_guard<std::mutex> lock(state.deviceMutex);
    if (!state.native.graphics || (graphics && graphics != state.native.graphics)) return;
    // Restore the device for native reset/destruction. Config writes are
    // protected separately at Main::WriteDisplayConfig, before destruction.
    auto* original = state.native.graphics;
    original->adapterOrdinal = state.native.adapter;
    *Dimps::Platform::D3D::GetConfiguredWidth(original) = state.native.width;
    *Dimps::Platform::D3D::GetConfiguredHeight(original) = state.native.height;
    *Dimps::Platform::D3D::GetConfiguredFullscreen(original) = state.native.fullscreen;
    *Dimps::Platform::D3D::GetConfiguredRefresh(original) = state.native.refresh;
    state.native.graphics = nullptr;
}

void RequestApply(HWND window) {
    if (!Enabled()||state.cancelled.load(std::memory_order_acquire)) return;
    if (!state.prepared.load(std::memory_order_acquire)) {
        spdlog::warn("Display: layout/device capability check failed; native display settings retained");
        return;
    }
    const UINT message = ApplyMessage();
    if (!message || !PostMessageW(window, message, 0, 0))
        Cancel("queue window setup",GetLastError());
}

bool WindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if(message==WM_DESTROY){
        if(state.bars)DestroyWindow(state.bars);
        state.bars=nullptr;state.applied=false;return false;
    }
    if (!Enabled() || !state.prepared.load(std::memory_order_acquire)||state.cancelled.load(std::memory_order_acquire)) return false;
    if (ApplyMessage() && message == ApplyMessage()) {
        Apply(window);
        return true;
    }
    if (message == WM_ACTIVATEAPP && state.applied && state.plan.selection.mode != Mode::Fullscreen) FocusChanged(window, wParam != 0);
    if (message == WM_SIZE && wParam == SIZE_MINIMIZED && state.bars) ShowWindow(state.bars, SW_HIDE);
    if (message == WM_SIZE && wParam == SIZE_RESTORED && state.applied && !state.applying) RequestApply(window);
    // The selected mode is fixed for this process. Do not let native Alt+Enter
    // switch it behind the layout. Alt+F4 remains available.
    if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && wParam == VK_RETURN && (lParam & (1L << 29))) return true;
    return false;
}
bool NativeConfig(NativeDisplayValues& values) {
    std::lock_guard<std::mutex> lock(state.deviceMutex);
    if(!state.native.captured)return false;
    values={state.native.width,state.native.height,state.native.fullscreen,state.native.refresh};return true;
}
#ifdef SF4E_DISPLAY_TESTING
void FailNextWindowOperation(WindowFault fault){state.fault=fault;}
#endif
} }
