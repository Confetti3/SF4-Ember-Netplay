#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include "BorderlessLayout.hxx"

namespace sf4e { namespace display {
enum class Mode { Native = 0, Windowed = 1, Borderless = 2, Fullscreen = 3 };
struct Resolution {
    int width = 0, height = 0, refresh = 0;
    bool operator==(const Resolution& b) const { return width == b.width && height == b.height && refresh == b.refresh; }
};
struct Preferences {
    Mode mode = Mode::Native;
    std::string monitor; // Windows display device name; empty selects the primary monitor.
    int width = 0, height = 0, refresh = 0; // Zero means automatic.
    bool Valid() const {
        if (static_cast<int>(mode) < 0 || static_cast<int>(mode) > 3 || monitor.size() > 128) return false;
        for (unsigned char c : monitor) if (c < 32 || c == 127) return false;
        if (refresh != 0 && (refresh < 24 || refresh > 1000)) return false;
        if (width == 0 || height == 0) return width == 0 && height == 0;
        return width >= 640 && width <= 16384 && height >= 360 && height <= 9216
            && width * 9 == height * 16;
    }
    bool operator==(const Preferences& b) const {
        return mode == b.mode && monitor == b.monitor && width == b.width && height == b.height && refresh == b.refresh;
    }
    bool operator!=(const Preferences& b) const { return !(*this == b); }
};
struct Monitor {
    std::string id, name;
    bool primary = false;
    int x = 0, y = 0, width = 0, height = 0;
    int workX = 0, workY = 0, workWidth = 0, workHeight = 0;
    unsigned adapter = 0;
    int refresh = 0;
    std::vector<Resolution> modes;
};
inline const Monitor* FindMonitor(const std::vector<Monitor>& monitors, const std::string& id) {
    if (monitors.empty()) return nullptr;
    for (const auto& monitor : monitors) if (!id.empty() && monitor.id == id) return &monitor;
    for (const auto& monitor : monitors) if (monitor.primary) return &monitor;
    return &monitors.front();
}
struct Selection {
    Mode mode = Mode::Native;
    int width = 0, height = 0, refresh = 0;
    Layout window;
    bool fallback = false;
};
// Resolve only advertised exclusive modes. If none can preserve 16:9, use
// borderless rather than asking D3D to create an unsupported fullscreen device.
inline Selection Resolve(const Preferences& pref, const Monitor& monitor) {
    Selection out;
    if (!pref.Valid() || pref.mode == Mode::Native) return out;
    out.mode = pref.mode;
    out.fallback = !pref.monitor.empty() && pref.monitor != monitor.id;
    const auto fit = Fit16By9(monitor.width, monitor.height);
    out.width = pref.width ? pref.width : fit.width;
    out.height = pref.height ? pref.height : fit.height;
    if (pref.mode == Mode::Fullscreen) {
        const Resolution* best = nullptr;
        for (const auto& mode : monitor.modes) {
            if (mode.width != out.width || mode.height != out.height || mode.refresh < 24) continue;
            if (pref.refresh && mode.refresh == pref.refresh) { best = &mode; break; }
            if (!best || mode.refresh == monitor.refresh || (best->refresh != monitor.refresh && mode.refresh > best->refresh)) best = &mode;
        }
        if (best) {
            out.refresh = best->refresh;
            out.fallback = out.fallback || (pref.refresh && out.refresh != pref.refresh);
        } else {
            out.mode = Mode::Borderless;
            out.fallback = true;
        }
    }
    if (out.mode == Mode::Windowed) {
        // Leave room for the title bar and taskbar, including on small screens.
        const int limitWidth = (std::min)(out.width, monitor.workWidth * 9 / 10);
        const int limitHeight = (std::min)(out.height, monitor.workHeight * 8 / 10);
        out.window = Fit16By9(limitWidth, limitHeight);
        out.window.x = monitor.workX + (monitor.workWidth - out.window.width) / 2;
        out.window.y = monitor.workY + (monitor.workHeight - out.window.height) / 2;
        if (!pref.width) { out.width = out.window.width; out.height = out.window.height; }
    } else if (out.mode == Mode::Borderless) {
        out.window = fit;
        out.window.x += monitor.x;
        out.window.y += monitor.y;
    } else {
        out.window.x = monitor.x; out.window.y = monitor.y;
        out.window.width = out.width; out.window.height = out.height;
    }
    return out;
}
} }
