#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include "DisplayBlock.hxx"
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

// The largest 16:9 area that fits a monitor, in whole steps of 16 by 9
// pixels so the picture is never stretched, centred on it. Borderless renders
// the game at this size. Empty when the monitor is smaller than one step.
struct Area {
    int x = 0, y = 0, width = 0, height = 0;
    bool Valid() const { return width > 0 && height > 0; }
};
inline Area Fit16By9(int width, int height) {
    const int step = (std::min)(width / 16, height / 9);
    if (step < 1) return {};
    return {(width - step * 16) / 2, (height - step * 9) / 2, step * 16, step * 9};
}

// DISPLAY/Resolution and DISPLAY/FullScreen with the player's Borderless on.
// Ember builds the device in a window at its monitor's 16:9 size. The game copies
// its device's settings into Main's (0x68F8E0, applying the brightness
// option) and sizes its window from Main's (0x782CF0), so both hold the
// Borderless mode, and the config write is given the player's own, as
// RenderingThreadOverride does for its flag. Any other mode in the device was
// put there by the game's display options and is the player's new choice.
// Choosing a window at exactly the Borderless size looks like no change, so
// the mode saved before stays.
class BorderlessOverride {
public:
    // Before the game builds its present parameters.
    void Apply(DisplayBlock& device, DisplayBlock& main, uint32_t width, uint32_t height) {
        const Mode borderless{width, height, 0};
        if (!applied_ || !(Read(device) == borderless)) requested_ = Read(device);
        applied_ = true;
        Put(borderless, device);
        Put(borderless, main);
    }
    // Gives both copies the player's mode back, so the next build is the
    // game's own, and leaves later writes to the game.
    void Restore(DisplayBlock& device, DisplayBlock& main) {
        if (!applied_) return;
        Put(requested_, device);
        Put(requested_, main);
        applied_ = false;
    }
    // Runs the game's config write with the player's mode in Main's copy and
    // puts the live mode back after. Unapplied, the mode is the game's.
    template <typename Write> void WriteRequested(DisplayBlock& main, Write&& write) const {
        if (!applied_) { write(); return; }
        const Mode live = Read(main);
        Put(requested_, main);
        write();
        Put(live, main);
    }
private:
    struct Mode {
        uint32_t width = 0, height = 0, fullScreen = 0;
        bool operator==(const Mode& other) const { return width == other.width && height == other.height && fullScreen == other.fullScreen; }
    };
    static Mode Read(const DisplayBlock& block) { return {block.width, block.height, block.fullScreen}; }
    static void Put(const Mode& mode, DisplayBlock& block) { block.width = mode.width; block.height = mode.height; block.fullScreen = mode.fullScreen; }
    Mode requested_;
    bool applied_ = false;
};

} }
