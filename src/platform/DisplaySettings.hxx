#pragma once
#include "../common/DisplayPreferences.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace platform {
std::vector<display::Monitor> DisplayMonitors();
bool DecodeDisplayPreferences(const nlohmann::json& value, display::Preferences& out);
nlohmann::json EncodeDisplayPreferences(const display::Preferences& value);
bool LoadDisplayPreferences(display::Preferences& out, std::string& error, const std::wstring& directory = {});
bool SaveDisplayPreferences(const display::Preferences& value, std::string& error, const std::wstring& directory = {});
// The launcher's copied selection wins over disk; the old test shortcut remains
// a one-launch borderless override. Neither changes the user's saved choice.
display::Preferences LaunchDisplayPreferences(std::string& error);
bool SetLaunchDisplayPreferences(const display::Preferences& value);
} }
