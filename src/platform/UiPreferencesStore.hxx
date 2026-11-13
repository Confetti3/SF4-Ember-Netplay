#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../common/UpdateChannel.hxx"
#include "../netplay/Profiles.hxx"

namespace sf4e { namespace platform {

std::string LoadLanguagePreference();
// On failure the file on disk is left intact and error receives an English
// diagnostic. It is detail for a developer, not display text: callers show a
// localized message of their own.
bool SaveLanguagePreference(std::string_view preference, std::string& error);
// The player chose "Don't show again" on the recommended game settings card.
bool GameSettingsCardHidden();
bool HideGameSettingsCardForever(std::string& error);
// The chosen update channel (stored as "stable", "prerelease" or "nightly"), or
// nothing while none was chosen: files written before the channel existed, and
// unknown stored text, included.
std::optional<updates::UpdateChannel> UpdateChannelPreference();
bool SaveUpdateChannelPreference(std::string_view channel, std::string& error);

// What the launch-time update check remembers: whether it runs at all (on until
// the player turns it off), when the last check that reached GitHub finished
// (unix seconds, 0 for never), and the release tag the player chose to skip.
// Files written before the check existed hold none of them.
struct UpdatePreferences {
    bool checkAtLaunch = true;
    std::uint64_t lastCheck = 0;
    std::string skipped;
};
UpdatePreferences LoadUpdatePreferences();
// Each saves its own field and leaves the rest of the file as it was, so the
// settings row and the check cannot overwrite each other. An empty tag clears
// the skip; on failure the file is left intact.
bool SaveCheckUpdatesAtLaunch(bool enabled, std::string& error);
bool SaveLastUpdateCheck(std::uint64_t time, std::string& error);
bool SaveSkippedUpdate(std::string_view tag, std::string& error);

// A tournament match Ember already told the player is ready to play, so a
// restart does not tell them again. `until` is the unix second after which the
// entry may be forgotten while its service says nothing about it: a month past
// the last list of that service that showed it, never the match's own expiry,
// which a new game can move later. 0 when this PC's clock was unknown: then only
// its service's list, or the bounds below, drop it.
struct AnnouncedMatch {
    std::string bridge, id;
    std::uint64_t until = 0;
};
// The bounds: per service a little more than the 50 matches a service lists at
// most, so its live matches always fit, and this many services. An entry its
// service's list no longer holds is gone before any bound applies.
constexpr std::size_t MaxAnnouncedPerService = 64, MaxAnnouncedServices = 8;
// Brings `matches` within the bounds, in place and in order otherwise. A service
// over its bound loses the entries whose list showed them longest ago (the
// lowest `until`, the earlier written on a tie); past the services bound, the
// service whose newest entry is oldest goes whole, never `keep`, the service
// being read. The file and its reader trim by this one rule.
void TrimAnnouncedMatches(std::vector<AnnouncedMatch>& matches, const std::string& keep = {});
// What the file holds, or nothing when it is absent or invalid. Entries that
// are malformed are dropped one by one, never the whole list.
std::vector<AnnouncedMatch> LoadAnnouncedMatches();
// Replaces the list, trimmed to the bounds. The language and the rest of the
// file stay as they were; on failure the file is left intact.
bool SaveAnnouncedMatches(const std::vector<AnnouncedMatch>& matches, std::string& error);

// The players this PC fought in public rooms lately, newest first: what Find a player lists as Recent opponents
// (netplay/Profiles.hxx, RecentList). Kept here on this PC and never sent. What the file holds, or nothing when it is
// absent or invalid: entries that are malformed are dropped one by one, never the whole list, and at most
// netplay::profiles::MaxRecent are read. Saving replaces the list, bounded the same way; the language and the rest of the
// file stay as they were, and on failure the file is left intact.
std::vector<netplay::profiles::Recent> LoadRecentOpponents();
bool SaveRecentOpponents(const std::vector<netplay::profiles::Recent>& entries, std::string& error);

namespace testing {
std::string LoadLanguagePreferenceFrom(const std::wstring& directory);
bool SaveLanguagePreferenceTo(const std::wstring& directory,
    std::string_view preference, std::string& error);
bool GameSettingsCardHiddenIn(const std::wstring& directory);
bool HideGameSettingsCardIn(const std::wstring& directory, std::string& error);
std::optional<updates::UpdateChannel> UpdateChannelPreferenceIn(const std::wstring& directory);
bool SaveUpdateChannelPreferenceTo(const std::wstring& directory, std::string_view channel, std::string& error);
UpdatePreferences LoadUpdatePreferencesFrom(const std::wstring& directory);
bool SaveCheckUpdatesAtLaunchTo(const std::wstring& directory, bool enabled, std::string& error);
bool SaveLastUpdateCheckTo(const std::wstring& directory, std::uint64_t time, std::string& error);
bool SaveSkippedUpdateTo(const std::wstring& directory, std::string_view tag, std::string& error);
std::vector<AnnouncedMatch> LoadAnnouncedMatchesFrom(const std::wstring& directory);
bool SaveAnnouncedMatchesTo(const std::wstring& directory, const std::vector<AnnouncedMatch>& matches, std::string& error);
std::vector<netplay::profiles::Recent> LoadRecentOpponentsFrom(const std::wstring& directory);
bool SaveRecentOpponentsTo(const std::wstring& directory, const std::vector<netplay::profiles::Recent>& entries, std::string& error);
}

} } // namespace sf4e::platform
