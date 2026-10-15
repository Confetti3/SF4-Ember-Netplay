#pragma once
// Kept apart from PlayerPreferences.hxx, which C++11 and C++14 targets include:
// the table is an inline variable, which needs C++17.
#include "PlayerPreferences.hxx"

namespace sf4e { namespace netplay {

// The on/off preferences saved as a plain true or false under their own key,
// which load as their PlayerPreferences default when a profile has none
// (BoolPreferenceJson.hxx). Auto delay, edition select and Public are saved
// otherwise and are not here.
template<class Preferences>
struct BoolPreferenceMember { const char* key; bool Preferences::* member; };
using BoolPreference = BoolPreferenceMember<PlayerPreferences>;
inline constexpr BoolPreference BoolPreferences[] = {
    {"showMatchHud", &PlayerPreferences::showMatchHud}, {"matchHudRaised", &PlayerPreferences::matchHudRaised},
    {"readySound", &PlayerPreferences::readySound}, {"trainingAutoReady", &PlayerPreferences::trainingAutoReady},
    {"watchedPicture", &PlayerPreferences::watchedPicture},
    {"matchFrameMeter", &PlayerPreferences::matchFrameMeter}, {"backgroundPlay", &PlayerPreferences::backgroundPlay},
    {"lowerInputLag", &PlayerPreferences::lowerInputLag}, {"lowerKeyboardLag", &PlayerPreferences::lowerKeyboardLag},
    {"fasterDisplay", &PlayerPreferences::fasterDisplay},
    {"controllerLayer", &PlayerPreferences::controllerLayer},
    {"recordWatched", &PlayerPreferences::recordWatched}, {"discordPresence", &PlayerPreferences::discordPresence},
    {"discordInvites", &PlayerPreferences::discordInvites}, {"shareSteamProfile", &PlayerPreferences::shareSteamProfile}, {"sendProblemReports", &PlayerPreferences::sendProblemReports},
};

} }
