#pragma once

// The player's preferences read from the saved settings (the launcher view
// SettingsStore::LoadLauncher gives), the one reader both games' runtimes use,
// and what is kept of the settings it could not read (SettingsGuard.hxx).

#include "ControllerMappings.hxx"
#include "InputDelayPreference.hxx"
#include "MatchHudPreference.hxx"
#include "PlayerPreferencesJson.hxx"
#include "RoomPreferences.hxx"
#include "SettingsGuardJson.hxx"

#include <nlohmann/json.hpp>

#include <string>

namespace sf4e { namespace netplay {

struct PreferencesRead {
    // "onlineRecord" held a record this build cannot read.
    bool recordInvalid = false;
    // A setting could not be read (a value of the wrong type): it keeps its
    // default and the file keeps its own value (PlayerPreferences::guard).
    bool fieldsFailed = false;
    // "roomDefaults" is not the room defaults this build reads.
    bool roomDefaultsInvalid = false;
};

namespace read_detail {
// Each setting is read on its own: one that cannot be read keeps its default
// and the settings after it still load.
template<class Read>
inline void Isolated(bool& failed, const Read& read) {
    try { read(); } catch (...) { failed = true; }
}
}

// Everything the saved settings hold, but the room defaults (ReadRoomPreferences),
// into `preferences`.
inline PreferencesRead ReadPlayerPreferences(const nlohmann::json& saved, PlayerPreferences& preferences) {
    PreferencesRead report;
    if (!saved.is_object()) return report;
    bool& failed = report.fieldsFailed;
    using read_detail::Isolated;
    Isolated(failed, [&] { if (saved.contains("onlineRecord") && !ReadProfileRecord(saved["onlineRecord"], preferences.record)) report.recordInvalid = true; });
    // The delay and Auto load from settings here, never from the launcher's
    // config copy of the delay (InputDelayPreference.hxx).
    Isolated(failed, [&] { ReadInputDelayPreference(saved, preferences); });
    const PlayerPreferences defaults;
    for (const auto& preference : BoolPreferences)
        Isolated(failed, [&] { preferences.*preference.member = saved.value(preference.key, defaults.*preference.member); });
    Isolated(failed, [&] { preferences.buttonPrompts = ReadButtonPrompts(saved); });
    Isolated(failed, [&] { preferences.controllerMappings = ReadControllerMappings(saved); });
    Isolated(failed, [&] { preferences.buttonLayouts = gamepads::ReadLayouts(saved, [](unsigned, gamepads::LayoutLoadError) {}); });
    Isolated(failed, [&] {
        const int hudSize = saved.value("matchHudSize", 1);
        preferences.matchHudSize = hudSize >= 0 && hudSize <= 2 ? hudSize : 1;
    });
    Isolated(failed, [&] {
        const int hudAnchor = saved.contains("matchHudAnchor") && saved["matchHudAnchor"].is_number_integer() ? saved["matchHudAnchor"].get<int>() : 0;
        preferences.matchHudAnchor = hudAnchor >= 0 && hudAnchor <= MaxMatchHudAnchor ? hudAnchor : 0;
    });
    Isolated(failed, [&] {
        const int pictureSpot = saved.contains("pictureSpot") && saved["pictureSpot"].is_number_integer() ? saved["pictureSpot"].get<int>() : 0;
        preferences.pictureSpot = pictureSpot >= 0 && pictureSpot <= 11 ? pictureSpot : 0;
    });
    Isolated(failed, [&] {
        // Before pictureMode there was the bool watchedPicture, off by default: everyone starts on Small now.
        const int pictureMode = saved.contains("pictureMode") && saved["pictureMode"].is_number_integer() ? saved["pictureMode"].get<int>() : PictureSmall;
        preferences.pictureMode = pictureMode >= PictureSmall && pictureMode <= PictureOff ? pictureMode : PictureSmall;
    });
    const auto text = [&](const char* key) { return saved.contains(key) && saved[key].is_string() ? saved[key].get<std::string>() : std::string(); };
    Isolated(failed, [&] { preferences.wornTitle = text("wornTitle"); preferences.wornIcon = text("wornIcon"); });
    Isolated(failed, [&] { for (int part = 0; part < GameThemePartCount; ++part) preferences.gameThemes[part] = text(GameThemeKeys[part]); });
    const auto percent = [&](const char* key, int fallback) { return saved.contains(key) && saved[key].is_number_integer() ? saved[key].get<int>() : fallback; };
    Isolated(failed, [&] {
        const int roomColumn = percent("roomColumn", DefaultRoomColumn), roomSplit = percent("roomSplit", DefaultRoomSplit);
        preferences.roomColumn = roomColumn >= MinRoomColumn && roomColumn <= MaxRoomColumn ? roomColumn : DefaultRoomColumn;
        preferences.roomSplit = roomSplit >= MinRoomSplit && roomSplit <= MaxRoomSplit ? roomSplit : DefaultRoomSplit;
    });
    Isolated(failed, [&] {
        const int hudLayout = saved.contains("matchHudLayout") && saved["matchHudLayout"].is_number_integer() ? saved["matchHudLayout"].get<int>() : 1;
        preferences.matchHudLayout = hudLayout >= 0 && hudLayout <= 1 ? hudLayout : 1;
    });
    Isolated(failed, [&] { preferences.matchHudNameOffset = ReadMatchHudNameOffset(saved); });
    Isolated(failed, [&] { preferences.country = ReadCountryPreference(saved); });
    Isolated(failed, [&] { preferences.area = ReadAreaPreference(saved); });
    Isolated(failed, [&] {
        const auto since = saved.find("shareReplaysSince");
        preferences.shareReplaysSince = since != saved.end() && since->is_number_unsigned() ? since->get<std::uint64_t>() : 0;
    });
    Isolated(failed, [&] { preferences.speech = ReadSpeechPreferences(saved); });
    Isolated(failed, [&] { ReadCardPreferences(saved, preferences); });
    Isolated(failed, [&] { preferences.roomFilter = ReadRoomFilter(saved); });
    Isolated(failed, [&] {
        const int volume = saved.value("readySoundVolume", 100);
        preferences.readySoundVolume = volume >= 10 && volume <= 100 ? volume / 10 * 10 : 100;
    });
    Isolated(failed, [&] { preferences.mainFighter = ReadMainFighter(saved); });
    Isolated(failed, [&] {
        const float scale = saved.value("interfaceScale", 1.f);
        preferences.interfaceScale = InterfaceScaleOrDefault(scale);
    });
    return report;
}

// All the saved settings into `preferences`, then what that left unread held
// (PlayerPreferences::guard), so a save leaves those as the file has them.
inline PreferencesRead ApplySavedSettings(const nlohmann::json& saved, PlayerPreferences& preferences) {
    auto report = ReadPlayerPreferences(saved, preferences);
    report.roomDefaultsInvalid = !ReadRoomPreferences(saved, preferences);
    preferences.guard = GuardUnreadKeys(saved, PlayerPreferencesValues(preferences));
    return report;
}

// The settings read after this run began, under the changes the player made
// meanwhile: each setting whose value moved since the settings were held is
// this run's and wins; every other is the file's.
inline nlohmann::json SavedUnderChanges(const nlohmann::json& saved, const PlayerPreferences& preferences) {
    nlohmann::json merged = saved;
    const auto values = PlayerPreferencesValues(preferences);
    for (auto entry = values.begin(); entry != values.end(); ++entry) {
        const auto held = preferences.guard.held.find(entry.key());
        if (held == preferences.guard.held.end() || held->second != entry.value().dump()) merged[entry.key()] = entry.value();
    }
    return merged;
}

} }
