#pragma once

// The player's preferences as the settings store keeps them (the launcher
// section of settings.json, SettingsStore::SaveLauncher): every game's
// Preferences writes this one document, and the runtime reads it back at
// start (NetplayRuntime's InitFromPayload).

#include "../common/ButtonLayoutJson.hxx"
#include "../common/GameCatalog.hxx"
#include "../common/ProductIdentity.hxx"
#include "BoolPreferenceJson.hxx"
#include "ButtonPromptPreference.hxx"
#include "CardPreferenceJson.hxx"
#include "CountryPreferenceJson.hxx"
#include "PlayerPreferences.hxx"
#include "ProfileRecordJson.hxx"
#include "PublicRoomFilterJson.hxx"
#include "RoomPreferences.hxx"
#include "SpeechPreferenceJson.hxx"

#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

inline nlohmann::json PlayerPreferencesJson(const PlayerPreferences& preferences) {
    nlohmann::json values={{"displayName", preferences.displayName}, {"mainFighter",preferences.mainFighter}, {"mainChosen", ember::ValidFighter(preferences.mainFighter)}, {"inputDelay", preferences.inputDelay}, {"autoInputDelay", preferences.autoInputDelay}, {"autoDelayStyle", preferences.autoDelayStyle},
        {"editionSelect", preferences.lobby.editionSelect ? 1 : 0}, {"roundCount", preferences.lobby.roundCount},
        {"roundTimeIntegral", preferences.lobby.roundTime},
        {"matchHudSize",preferences.matchHudSize},{"matchHudAnchor",preferences.matchHudAnchor},{"pictureSpot",preferences.pictureSpot},{"pictureMode",preferences.pictureMode},{"wornTitle",preferences.wornTitle},{"wornIcon",preferences.wornIcon},{"roomColumn",preferences.roomColumn},{"roomSplit",preferences.roomSplit},{"matchHudLayout",preferences.matchHudLayout},{"matchHudNameOffset",preferences.matchHudNameOffset},{"readySoundVolume",preferences.readySoundVolume},{"shareReplaysSince",preferences.shareReplaysSince},
        {"interfaceScale", preferences.interfaceScale}, {"roomDefaults", RoomPreferences(preferences)}};
    WriteBoolPreferences(preferences, values);
    WriteButtonPrompts(preferences, values);
    WriteCountryPreference(preferences, values);
    WriteAreaPreference(preferences, values);
    WriteSpeechPreferences(preferences, values);
    WriteCardPreferences(preferences, values);
    WriteRoomFilter(preferences, values);
    for (int part = 0; part < GameThemePartCount; ++part) values[GameThemeKeys[part]] = preferences.gameThemes[part];
    values["controllerMappings"] = preferences.controllerMappings;
    values["buttonLayouts"] = gamepads::LayoutsToJson(preferences.buttonLayouts);
    if (preferences.record.available) values["onlineRecord"] = ProfileRecordJson(preferences.record);
    return values;
}

// The saved main ("mainFighter"): one of the catalog's fighters, else the
// main a player has before choosing one (-1 where the product's main starts
// unset, else the roster's first fighter: ember::UnsetMain). A save says the
// main was chosen ("mainChosen"). Builds before it wrote the roster's first
// fighter as everyone's main, chosen or not: where the main starts unset (SFV
// Ember), a 0 saved without "mainChosen" is that default and reads as no
// main, and a player who had chosen that fighter chooses it once more.
inline int MainFighterOf(const nlohmann::json& saved, bool mainStartsUnset) {
    const int unset = mainStartsUnset ? -1 : 0;
    const auto main = saved.find("mainFighter");
    if (main == saved.end() || !main->is_number_integer() || !ember::ValidFighter(main->get<std::int64_t>())) return unset;
    const int fighter = main->get<int>();
    const auto chosen = saved.find("mainChosen");
    const bool marked = chosen != saved.end() && chosen->is_boolean();
    return mainStartsUnset && fighter == 0 && !marked ? unset : fighter;
}
// The product's (Branding::mainStartsUnset).
inline int ReadMainFighter(const nlohmann::json& saved) { return MainFighterOf(saved, ember::Product().branding.mainStartsUnset); }

} } // namespace sf4e::netplay
