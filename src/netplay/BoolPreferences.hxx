#pragma once
// Kept apart from PlayerPreferences.hxx, which C++11 and C++14 targets include:
// the table is an inline variable, which needs C++17.
#include "PlayerPreferences.hxx"

namespace sf4e { namespace netplay {

// The on/off preferences saved as a plain true or false under their own key,
// which load as their PlayerPreferences default when a profile has none
// (BoolPreferenceJson.hxx). Auto delay, edition select and Public are saved
// otherwise and are not here.
struct BoolPreference { const char* key; bool PlayerPreferences::* member; };
inline constexpr BoolPreference BoolPreferences[] = {
    {"showMatchHud", &PlayerPreferences::showMatchHud}, {"matchHudRaised", &PlayerPreferences::matchHudRaised},
    {"readySound", &PlayerPreferences::readySound}, {"trainingAutoReady", &PlayerPreferences::trainingAutoReady},
    {"matchFrameMeter", &PlayerPreferences::matchFrameMeter}, {"backgroundPlay", &PlayerPreferences::backgroundPlay},
    {"recordWatched", &PlayerPreferences::recordWatched}, {"discordPresence", &PlayerPreferences::discordPresence},
    {"discordInvites", &PlayerPreferences::discordInvites},
};

} }
