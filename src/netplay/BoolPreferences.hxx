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
    {"showShortcutHint", &PlayerPreferences::showShortcutHint},
    {"readySound", &PlayerPreferences::readySound}, {"trainingAutoReady", &PlayerPreferences::trainingAutoReady},
    {"classEmblems", &PlayerPreferences::classEmblems}, {"tvMode", &PlayerPreferences::tvMode},
    {"welcomed", &PlayerPreferences::welcomed},
    {"roomShowMembers", &PlayerPreferences::roomShowMembers}, {"roomShowChat", &PlayerPreferences::roomShowChat},
    {"chatFilter", &PlayerPreferences::chatFilter},
    {"matchFrameMeter", &PlayerPreferences::matchFrameMeter}, {"backgroundPlay", &PlayerPreferences::backgroundPlay}, {"endlessLobbyMusic", &PlayerPreferences::endlessLobbyMusic},
    {"lowerInputLag", &PlayerPreferences::lowerInputLag}, {"lowerKeyboardLag", &PlayerPreferences::lowerKeyboardLag},
    {"fasterDisplay", &PlayerPreferences::fasterDisplay}, {"borderless", &PlayerPreferences::borderless},
    {"controllerLayer", &PlayerPreferences::controllerLayer},
    {"recordWatched", &PlayerPreferences::recordWatched}, {"shareReplays", &PlayerPreferences::shareReplays}, {"discordPresence", &PlayerPreferences::discordPresence},
    {"exportNames", &PlayerPreferences::exportNames}, {"exportLine", &PlayerPreferences::exportLine}, {"exportSet", &PlayerPreferences::exportSet},
    {"exportMark", &PlayerPreferences::exportMark}, {"exportMeter", &PlayerPreferences::exportMeter},
    // Send problem reports is saved as sendProblemReportsWithDump since reports
    // carry the small dump, so everyone who agreed to logs only is asked again.
    // Stable 1.5.0 ships this key and retires the old sendProblemReports
    // (ObsoleteKeys in SettingsStore.cxx). An older Stable sharing
    // %APPDATA%\sf4e then reads its setting as off and asks again; it never
    // sends without asking.
    {"discordInvites", &PlayerPreferences::discordInvites}, {"shareSteamProfile", &PlayerPreferences::shareSteamProfile}, {"sendProblemReportsWithDump", &PlayerPreferences::sendProblemReports},
};

} }
