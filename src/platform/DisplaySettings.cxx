#include "DisplaySettings.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../common/EnvFlag.hxx"
#include <windows.h>
#include <d3d9.h>
#include <tuple>

namespace sf4e { namespace platform {
namespace {
std::string Utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, &out[0], size, nullptr, nullptr);
    out.pop_back(); return out;
}
bool Integer(const nlohmann::json& j, const char* key, int& out) {
    if (!j.contains(key) || !j[key].is_number_integer()) return false;
    const auto n = j[key].get<std::int64_t>();
    if (n < 0 || n > 16384) return false;
    out = static_cast<int>(n); return true;
}
}
std::vector<display::Monitor> DisplayMonitors() {
    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::vector<display::Monitor> out;
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (d3d) {
        for (UINT adapter = 0; adapter < d3d->GetAdapterCount(); ++adapter) {
            MONITORINFOEXW info = {}; info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(d3d->GetAdapterMonitor(adapter), &info)) continue;
            display::Monitor monitor;
            monitor.id = Utf8(info.szDevice);
            monitor.name = monitor.id;
            DISPLAY_DEVICEW device = {}; device.cb = sizeof(device);
            if (EnumDisplayDevicesW(info.szDevice, 0, &device, 0) && device.DeviceString[0]) monitor.name = Utf8(device.DeviceString);
            monitor.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            monitor.adapter = adapter;
            monitor.x = info.rcMonitor.left; monitor.y = info.rcMonitor.top;
            monitor.width = info.rcMonitor.right - info.rcMonitor.left;
            monitor.height = info.rcMonitor.bottom - info.rcMonitor.top;
            monitor.workX = info.rcWork.left; monitor.workY = info.rcWork.top;
            monitor.workWidth = info.rcWork.right - info.rcWork.left;
            monitor.workHeight = info.rcWork.bottom - info.rcWork.top;
            D3DDISPLAYMODE current = {};
            if (SUCCEEDED(d3d->GetAdapterDisplayMode(adapter, &current))) monitor.refresh = current.RefreshRate;
            for (UINT i = 0; i < d3d->GetAdapterModeCount(adapter, D3DFMT_X8R8G8B8); ++i) {
                D3DDISPLAYMODE mode = {};
                if (SUCCEEDED(d3d->EnumAdapterModes(adapter, D3DFMT_X8R8G8B8, i, &mode))
                    && mode.Width >= 640 && mode.Width <= 16384 && mode.Height >= 360 && mode.Height <= 9216
                    && mode.Width * 9 == mode.Height * 16 && mode.RefreshRate >= 24 && mode.RefreshRate <= 1000)
                    monitor.modes.push_back({static_cast<int>(mode.Width), static_cast<int>(mode.Height), static_cast<int>(mode.RefreshRate)});
            }
            std::sort(monitor.modes.begin(), monitor.modes.end(), [](const display::Resolution& a, const display::Resolution& b) {
                return std::tie(a.width, a.height, a.refresh) < std::tie(b.width, b.height, b.refresh);
            });
            monitor.modes.erase(std::unique(monitor.modes.begin(), monitor.modes.end()), monitor.modes.end());
            out.push_back(std::move(monitor));
        }
        d3d->Release();
    }
    if (previous) SetThreadDpiAwarenessContext(previous);
    return out;
}
bool DecodeDisplayPreferences(const nlohmann::json& value, display::Preferences& out) {
    try {
        if (!value.is_object() || !value.contains("version") || !value["version"].is_number_integer() || value["version"] != 1
            || !value.contains("monitor") || !value["monitor"].is_string()) return false;
        display::Preferences decoded;
        int mode = 0;
        if (!Integer(value, "mode", mode) || !Integer(value, "width", decoded.width) || !Integer(value, "height", decoded.height)
            || !Integer(value, "refresh", decoded.refresh)) return false;
        decoded.mode = static_cast<display::Mode>(mode); decoded.monitor = value["monitor"].get<std::string>();
        if (!decoded.Valid()) return false;
        out = std::move(decoded); return true;
    } catch (...) { return false; }
}
nlohmann::json EncodeDisplayPreferences(const display::Preferences& value) {
    return {{"version", 1}, {"mode", static_cast<int>(value.mode)}, {"monitor", value.monitor},
        {"width", value.width}, {"height", value.height}, {"refresh", value.refresh}};
}
bool LoadDisplayPreferences(display::Preferences& out, std::string& error, const std::wstring& directory) {
    nlohmann::json settings;
    if (!netplay::SettingsStore(directory.empty() ? netplay::SettingsStore::DefaultDirectory() : directory).LoadLauncher(settings, error)) return false;
    if (!settings.contains("display")) { out = {}; return true; }
    if (!DecodeDisplayPreferences(settings["display"], out)) { error = "Invalid display preferences; native game settings will be used."; return false; }
    return true;
}
bool SaveDisplayPreferences(const display::Preferences& value, std::string& error, const std::wstring& directory) {
    if (!value.Valid()) { error = "Invalid display preferences."; return false; }
    return netplay::SettingsStore(directory.empty() ? netplay::SettingsStore::DefaultDirectory() : directory).SaveLauncher({{"display", EncodeDisplayPreferences(value)}}, error);
}
display::Preferences LaunchDisplayPreferences(std::string& error) {
    display::Preferences out;
    char value[1024] = {};
    const DWORD length = GetEnvironmentVariableA("SF4E_DISPLAY_CONFIG", value, sizeof(value));
    if (length) {
        if (length >= sizeof(value) || !DecodeDisplayPreferences(nlohmann::json::parse(value, nullptr, false), out))
            error = "Invalid launch display selection; native game settings will be used.";
        return out;
    }
    if (EnvFlag("SF4E_BORDERLESS_TEST")) { out.mode = display::Mode::Borderless; return out; }
    LoadDisplayPreferences(out, error);
    return out;
}
bool SetLaunchDisplayPreferences(const display::Preferences& value) {
    return value.Valid() && SetEnvironmentVariableA("SF4E_DISPLAY_CONFIG", EncodeDisplayPreferences(value).dump().c_str());
}
} }
