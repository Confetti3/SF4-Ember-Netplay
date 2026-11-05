#pragma once

#include "../common/Country.hxx"
#include "../common/Localization.hxx"

#include <string>
#include <string_view>
#include <vector>

namespace sf4e { namespace platform {
std::vector<std::string> WindowsUiLanguages();
// The Steam client's language name, which Steam reports to a game that has no
// language of its own. Empty when Steam has not recorded one.
std::string SteamClientLanguage();
// Records the game's language, which "auto" follows in this process, and logs
// nothing: callers log the result. `code` is a game language code as
// ResolveLocale reads it (USF4's codes, GameLanguage.hxx); empty forgets it.
void SetGameLanguage(std::string code);
// The recorded game language code, or empty when unknown.
std::string GameLanguage();
// ResolveLocale with this process's game language and Windows UI languages.
loc::Locale ResolveUiLocale(std::string_view preference);
// The country of the user's Region setting in Windows (GetUserGeoID and its
// GEO_ISO2), which Automatic shows. None when Windows names none, or names one
// the country table lacks.
CountryCode WindowsCountry();
} }
