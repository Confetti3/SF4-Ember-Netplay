#include "sf4e__Borderless.hxx"

#include <atomic>

#include "spdlog/spdlog.h"

#include "../Dimps/Dimps__Platform.hxx"
#include "../common/GameDisplayConfig.hxx"

namespace sf4e { namespace borderless {
namespace {
enum class Phase { Off, Chosen, Planned };
struct State {
	std::atomic<Phase> phase{Phase::Off};
	// Written once before the phase becomes Planned, then only read: the
	// monitor and the 16:9 area of it the game renders to.
	RECT monitor = {};
	gameconfig::Area area;
	// The player's mode while the Borderless one is in the device.
	gameconfig::BorderlessOverride mode;
	// The game's window thread only: the black bars beside a 16:9 area on a
	// monitor of another shape, and a layout asked for while minimized.
	HWND bars = nullptr;
	bool layoutPending = false;
} state;

void Fail(const char* what, DWORD error) {
	if (state.phase.exchange(Phase::Off) == Phase::Off) return;
	spdlog::error("Borderless: {} (Win32 {}); the game keeps its own mode and window", what, error);
}

// Registered, not WM_APP: the window procedure is the game's own.
UINT LayoutMessage() {
	static const UINT message = RegisterWindowMessageW(L"SF4Ember.Borderless.Layout");
	return message;
}

// The game fills a picture of any shape and crops what does not fit (its
// view takes the backbuffer's ratio under Aspect AUTO, 0x780480), so it is
// given a 16:9 one and the rest of the monitor is bars.
void Plan(Dimps::Platform::D3D* device) {
	const HMONITOR monitor = device->lpD3D ? device->lpD3D->GetAdapterMonitor(device->adapterOrdinal) : nullptr;
	MONITORINFO info = {sizeof(info)};
	if (!monitor || !GetMonitorInfoW(monitor, &info)) { Fail("no monitor for the game's adapter", GetLastError()); return; }
	state.area = gameconfig::Fit16By9(info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top);
	if (!state.area.Valid()) { Fail("the monitor is too small", ERROR_INVALID_DATA); return; }
	state.monitor = info.rcMonitor;
	state.phase.store(Phase::Planned);
}

LRESULT CALLBACK BarsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
	if (message == WM_NCHITTEST) return HTTRANSPARENT;
	return DefWindowProcW(window, message, wParam, lParam);
}

// Black over the whole monitor with the game's area cut out. Owned by the
// game's window, so it stays above it, and the hole keeps it off the picture
// and the game's input.
HWND CreateBars(HWND owner) {
	const RECT& monitor = state.monitor;
	const gameconfig::Area& area = state.area;
	const int width = monitor.right - monitor.left, height = monitor.bottom - monitor.top;
	WNDCLASSW windowClass = {};
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.lpszClassName = L"SF4Ember.Borderless.Bars";
	windowClass.lpfnWndProc = BarsProc;
	windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
	if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
	HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.lpszClassName, L"", WS_POPUP,
		monitor.left, monitor.top, width, height, owner, nullptr, windowClass.hInstance, nullptr);
	if (!window) return nullptr;
	HRGN region = CreateRectRgn(0, 0, width, height);
	HRGN hole = CreateRectRgn(area.x, area.y, area.x + area.width, area.y + area.height);
	// Windows owns the region once SetWindowRgn takes it.
	const bool cut = region && hole && CombineRgn(region, region, hole, RGN_DIFF) != ERROR && SetWindowRgn(window, region, TRUE);
	if (hole) DeleteObject(hole);
	if (!cut) {
		if (region) DeleteObject(region);
		DestroyWindow(window);
		return nullptr;
	}
	return window;
}

// The bars show while the game is in front. Covering the monitor with them,
// or the window covering it alone on a 16:9 monitor, keeps the taskbar behind.
void ShowBars(HWND window, bool active) {
	if (state.bars) ShowWindow(state.bars, active && !IsIconic(window) ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void Layout(HWND window) {
	state.layoutPending = IsIconic(window) != FALSE;
	if (state.layoutPending) return;
	const RECT& monitor = state.monitor;
	const gameconfig::Area& area = state.area;
	const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE), exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
	SetWindowLongPtrW(window, GWL_STYLE, (style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_POPUP);
	SetWindowLongPtrW(window, GWL_EXSTYLE, exStyle & ~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
	if (!SetWindowPos(window, nullptr, monitor.left + area.x, monitor.top + area.y, area.width, area.height,
		SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED)) {
		Fail("the window could not be laid out", GetLastError());
		return;
	}
	const bool wider = area.width < monitor.right - monitor.left || area.height < monitor.bottom - monitor.top;
	if (wider && !state.bars && !(state.bars = CreateBars(window)))
		spdlog::warn("Borderless: the black bars could not be made (Win32 {})", GetLastError());
	spdlog::info("Borderless: {}x{} at {},{} on a {}x{} monitor", area.width, area.height,
		monitor.left + area.x, monitor.top + area.y, monitor.right - monitor.left, monitor.bottom - monitor.top);
	ShowBars(window, GetForegroundWindow() == window);
}
}

void Choose(bool chosen) {
	state.phase.store(chosen ? Phase::Chosen : Phase::Off);
	// Before the game makes its window (ApplyStartupParams runs first), so the
	// window and the monitor are in physical pixels and a scaled desktop does
	// not stretch the picture. The game is not DPI aware without it.
	if (chosen && !SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
		spdlog::warn("Borderless: the game could not be made DPI aware ({}); a scaled desktop may stretch it", GetLastError());
}

void BeforePresentParameters(Dimps::Platform::D3D* device, Dimps::Platform::Main* main) {
	if (state.phase.load() == Phase::Chosen) Plan(device);
	auto& display = *Dimps::Platform::D3D::GetDisplay(device);
	auto& saved = *Dimps::Platform::Main::GetDisplay(main);
	if (state.phase.load() != Phase::Planned) { state.mode.Restore(display, saved); return; }
	state.mode.Apply(display, saved, static_cast<uint32_t>(state.area.width), static_cast<uint32_t>(state.area.height));
	// Handled once the game's display change returns, after it has sized the
	// window itself (0x782CF0 comes before the device build).
	if (!PostMessageW(device->hFocusWindow, LayoutMessage(), 0, 0)) Fail("the window layout could not be queued", GetLastError());
}

void WriteConfig(Dimps::Platform::Main* main, const std::function<void()>& write) {
	state.mode.WriteRequested(*Dimps::Platform::Main::GetDisplay(main), write);
}

bool WindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_DESTROY && state.bars) { DestroyWindow(state.bars); state.bars = nullptr; }
	if (state.phase.load() != Phase::Planned) return false;
	if (message == LayoutMessage()) { Layout(window); return true; }
	if (message == WM_SIZE && wParam == SIZE_RESTORED && state.layoutPending) Layout(window);
	if (message == WM_ACTIVATEAPP) ShowBars(window, wParam != 0);
	// Alt+Enter would switch the game to exclusive fullscreen behind the
	// layout. Alt+F4 still closes it.
	return (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && wParam == VK_RETURN && (lParam & (1L << 29));
}
} }
