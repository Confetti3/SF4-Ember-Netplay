#pragma once

#include <string>
#include <string_view>

namespace sf4e { namespace platform {

std::string LoadLanguagePreference();
// On failure the file on disk is left intact and error receives an English
// diagnostic. It is detail for a developer, not display text: callers show a
// localized message of their own.
bool SaveLanguagePreference(std::string_view preference, std::string& error);
// The player chose "Don't show again" on the recommended game settings card.
bool GameSettingsCardHidden();
bool HideGameSettingsCardForever(std::string& error);

namespace testing {
std::string LoadLanguagePreferenceFrom(const std::wstring& directory);
bool SaveLanguagePreferenceTo(const std::wstring& directory,
    std::string_view preference, std::string& error);
bool GameSettingsCardHiddenIn(const std::wstring& directory);
bool HideGameSettingsCardIn(const std::wstring& directory, std::string& error);
}

} } // namespace sf4e::platform
