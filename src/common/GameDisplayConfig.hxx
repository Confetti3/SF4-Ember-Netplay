#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace sf4e { namespace gameconfig {

// The settings of the game's own config.ini that rollback depends on and the
// player must change. VSync is not here: the mod forces it off.
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

} }
