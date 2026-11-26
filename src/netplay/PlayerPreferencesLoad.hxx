#pragma once
#include "PlayerPreferences.hxx"
#include "BoolPreferenceJson.hxx"
#include "ButtonPromptPreference.hxx"
#include "CardPreferenceJson.hxx"
#include "ControllerMappings.hxx"
#include "CountryPreferenceJson.hxx"
#include "MatchHudPreference.hxx"
#include "PublicRoomFilterJson.hxx"
#include "SpeechPreferenceJson.hxx"
#include "FightChatPreferenceJson.hxx"
#include "../common/ButtonLayoutJson.hxx"
#include <cstdint>
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {

namespace preference_load {
inline int ClampedInt(const nlohmann::json& saved, const char* key, int fallback, int low, int high) {
    const int value = saved.contains(key) && saved[key].is_number_integer() ? saved[key].get<int>() : fallback;
    return value >= low && value <= high ? value : fallback;
}
inline std::string Text(const nlohmann::json& saved, const char* key) {
    return saved.contains(key) && saved[key].is_string() ? saved[key].get<std::string>() : std::string();
}
}

// The interface preferences from a saved profile. Each setting loads on its
// own: one that cannot be read (a value of the wrong type) keeps its default
// and the settings after it still load. Returns false when any setting could
// not be read.
inline bool LoadPlayerPreferences(const nlohmann::json& saved, PlayerPreferences& preferences) {
    using namespace preference_load;
    bool allRead = true;
    const auto setting = [&](auto&& read) {
        try { read(); } catch (...) { allRead = false; }
    };
    for (const auto& boolean : BoolPreferences) setting([&] { ReadBoolPreference(saved, preferences, boolean); });
    setting([&] { preferences.buttonPrompts = ReadButtonPrompts(saved); });
    setting([&] { preferences.controllerMappings = ReadControllerMappings(saved); });
    setting([&] { preferences.buttonLayouts = gamepads::ReadLayouts(saved, [](unsigned, gamepads::LayoutLoadError) {}); });
    setting([&] {
        const int hudSize = saved.value("matchHudSize", 1);
        preferences.matchHudSize = hudSize >= 0 && hudSize <= 2 ? hudSize : 1;
    });
    setting([&] { preferences.matchHudAnchor = ClampedInt(saved, "matchHudAnchor", 0, 0, MaxMatchHudAnchor); });
    setting([&] { preferences.pictureSpot = ClampedInt(saved, "pictureSpot", 0, 0, 11); });
    // Before pictureMode there was the bool watchedPicture, off by default: everyone starts on Small now.
    setting([&] { preferences.pictureMode = ClampedInt(saved, "pictureMode", PictureSmall, PictureSmall, PictureOff); });
    setting([&] { preferences.wornTitle = Text(saved, "wornTitle"); });
    setting([&] { preferences.wornIcon = Text(saved, "wornIcon"); });
    for (int part = 0; part < GameThemePartCount; ++part)
        setting([&] { preferences.gameThemes[part] = Text(saved, GameThemeKeys[part]); });
    setting([&] { preferences.roomColumn = ClampedInt(saved, "roomColumn", DefaultRoomColumn, MinRoomColumn, MaxRoomColumn); });
    setting([&] { preferences.roomSplit = ClampedInt(saved, "roomSplit", DefaultRoomSplit, MinRoomSplit, MaxRoomSplit); });
    setting([&] { preferences.matchHudLayout = ClampedInt(saved, "matchHudLayout", 1, 0, 1); });
    setting([&] { preferences.matchHudNameOffset = ReadMatchHudNameOffset(saved); });
    setting([&] { preferences.country = ReadCountryPreference(saved); });
    setting([&] { preferences.area = ReadAreaPreference(saved); });
    setting([&] {
        const auto since = saved.find("shareReplaysSince");
        preferences.shareReplaysSince = since != saved.end() && since->is_number_unsigned() ? since->get<std::uint64_t>() : 0;
    });
    setting([&] { preferences.speech = ReadSpeechPreferences(saved); });
    setting([&] { preferences.fightChat = ReadFightChatPreferences(saved); });
    setting([&] { ReadCardPreferences(saved, preferences); });
    setting([&] { preferences.roomFilter = ReadRoomFilter(saved); });
    setting([&] {
        const int volume = saved.value("readySoundVolume", 100);
        preferences.readySoundVolume = volume >= 10 && volume <= 100 ? volume / 10 * 10 : 100;
    });
    setting([&] { preferences.mainFighter = ClampedInt(saved, "mainFighter", 0, 0, selection::FighterCount - 1); });
    setting([&] { preferences.interfaceScale = InterfaceScaleOrDefault(saved.value("interfaceScale", 1.f)); });
    return allRead;
}

} }
