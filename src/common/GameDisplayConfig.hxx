#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace sf4e { namespace gameconfig {

// The settings of the game's own config.ini that rollback depends on and the
// player must change. VSync is not here: the mod forces it off. Nor is
// RenderingThread, which Faster display turns off (RenderingThreadOverride).
// An empty value means the key was absent; it is not reported as a mismatch.
struct DisplaySettings {
    std::string frameRate, msaa;
    bool FrameRateOk() const { return frameRate.empty() || frameRate == "FIXED"; }
    bool MsaaOk() const { return msaa.empty() || msaa == "NONE"; }
    bool Recommended() const { return FrameRateOk() && MsaaOk(); }
};

// Keys are unique across the file's sections, so sections are ignored.
inline DisplaySettings ParseDisplaySettings(std::string_view ini) {
    DisplaySettings out;
    while (!ini.empty()) {
        const auto end = ini.find('\n');
        std::string_view line = ini.substr(0, end);
        ini.remove_prefix(end == std::string_view::npos ? ini.size() : end + 1);
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        // Values are single words, so whitespace anywhere is noise and case
        // is not significant: "MSAA = none" and "MSAA=NONE" are the same file.
        const auto normalize = [](std::string_view text) {
            std::string value(text);
            value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }), value.end());
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return value;
        };
        const auto key = normalize(line.substr(0, equals));
        if (key == "FRAMERATE") out.frameRate = normalize(line.substr(equals + 1));
        else if (key == "MSAA") out.msaa = normalize(line.substr(equals + 1));
    }
    return out;
}

// SYSTEM/RenderingThread. ON draws each frame on a second thread while the
// game thread simulates the next one, so the picture reaches the screen a
// frame later. With the player's Faster display on, Ember turns the flag off
// before the game first stores it; with it off, the game keeps what
// config.ini says. The player's file is left as it was either way: the game
// writes the flag back into config.ini (on exit and from its display
// options), and that write is given the player's own value
// (sf4e__Platform.cxx). Parts of the game latch the flag during startup, so
// it holds for the session and must not change after.
class RenderingThreadOverride {
public:
    // The flag as config.ini set it, before the game stores it: remembered,
    // and turned off when fasterDisplay is on.
    void Apply(bool fasterDisplay, int& startup) {
        requested_ = startup; applied_ = true; enforced_ = fasterDisplay;
        if (enforced_) startup = 0;
    }
    // Applies the override to the startup parameters' flag, then runs the
    // game's startup routine, which copies the flag into Main and lets the
    // graphics device latch it. The order is the point: the override must be
    // in place before the routine reads the flag, never after.
    template <typename Original> decltype(auto) ApplyThenStart(bool fasterDisplay, int& startup, Original&& original) {
        Apply(fasterDisplay, startup);
        return original();
    }
    // Runs the game's config write with the player's value in the flag and
    // puts the enforced one back after. Unenforced, the flag is the game's.
    template <typename Write> void WriteRequested(int& live, Write&& write) const {
        if (!enforced_) { write(); return; }
        const int enforced = live;
        live = requested_;
        write();
        live = enforced;
    }
    bool Applied() const { return applied_; }
    // Whether the game draws on its own thread this session.
    bool RenderThread() const { return requested_ != 0 && !enforced_; }
    // The startup log line after "Display: ".
    std::string Describe() const {
        const char* asked = requested_ ? "ON" : "OFF";
        return enforced_ ? std::string("rendering thread forced off (Faster display on; game setting asked for ") + asked + ")"
            : std::string("rendering thread left to the game (Faster display off; RenderingThread=") + asked + ")";
    }
private:
    int requested_ = 0;
    bool applied_ = false, enforced_ = false;
};

} }
