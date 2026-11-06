#pragma once

// The player's preferences as the settings store keeps them (the launcher
// section of settings.json, SettingsStore::SaveLauncher): every game's
// Preferences writes this one document, and the runtime reads it back at
// start (NetplayRuntime's InitFromPayload).

#include "../common/ButtonLayoutJson.hxx"
#include "BoolPreferenceJson.hxx"
#include "ButtonPromptPreference.hxx"
#include "CountryPreferenceJson.hxx"
#include "PlayerPreferences.hxx"
#include "ProfileRecordJson.hxx"
#include "PublicRoomFilterJson.hxx"
#include "RoomPreferences.hxx"
#include "SpeechPreferenceJson.hxx"

#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

inline nlohmann::json PlayerPreferencesJson(const PlayerPreferences& preferences) {
    nlohmann::json values={{"displayName", preferences.displayName}, {"mainFighter",preferences.mainFighter}, {"inputDelay", preferences.inputDelay}, {"autoInputDelay", preferences.autoInputDelay}, {"autoDelayStyle", preferences.autoDelayStyle},
        {"editionSelect", preferences.lobby.editionSelect ? 1 : 0}, {"roundCount", preferences.lobby.roundCount},
        {"roundTimeIntegral", preferences.lobby.roundTime},
        {"matchHudSize",preferences.matchHudSize},{"matchHudAnchor",preferences.matchHudAnchor},{"pictureSpot",preferences.pictureSpot},{"pictureMode",preferences.pictureMode},{"wornTitle",preferences.wornTitle},{"wornIcon",preferences.wornIcon},{"roomColumn",preferences.roomColumn},{"roomSplit",preferences.roomSplit},{"matchHudLayout",preferences.matchHudLayout},{"matchHudNameOffset",preferences.matchHudNameOffset},{"readySoundVolume",preferences.readySoundVolume},{"shareReplaysSince",preferences.shareReplaysSince},
        {"interfaceScale", preferences.interfaceScale}, {"roomDefaults", RoomPreferences(preferences)}};
    WriteBoolPreferences(preferences, values);
    WriteButtonPrompts(preferences, values);
    WriteCountryPreference(preferences, values);
    WriteSpeechPreferences(preferences, values);
    WriteRoomFilter(preferences, values);
    values["controllerMappings"] = preferences.controllerMappings;
    values["buttonLayouts"] = gamepads::LayoutsToJson(preferences.buttonLayouts);
    if (preferences.record.available) values["onlineRecord"] = ProfileRecordJson(preferences.record);
    return values;
}

} } // namespace sf4e::netplay
