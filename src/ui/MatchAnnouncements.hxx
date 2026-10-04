#pragma once
#include "../netplay/TournamentStatus.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {
// The tournament matches Ember already told the player are ready to play: one
// entry per service and match, kept across restarts (the UI preferences file)
// so a match is announced once, not at every launch. Only a list its service
// answered forgets its entries, in the same step that brings them up to date: a
// match it no longer lists goes, and one it lists stays, whatever its expiry
// says, since a new game can move that later. While that service's list loads,
// fails or is not asked for, its entries stay, for a month past the last list
// that showed them; past that they go, and a match still live then is told again.
// The bounds (UiPreferencesStore) hold every match one service lists, and put
// off the service heard from longest ago first.
class MatchAnnouncements {
public:
    using Loader = std::function<std::vector<platform::AnnouncedMatch>()>;
    using Saver = std::function<bool(const std::vector<platform::AnnouncedMatch>&)>;
    // Where the entries are kept; the platform store unless a test supplies its own.
    void SetStore(Loader loader, Saver saver) { load_ = std::move(loader); save_ = std::move(saver); }
    // Brings the entries up to date with `list`, `unixNow` being this PC's clock
    // (0 when unknown), and notes its playable matches: true when one is new.
    bool Take(const netplay::tournament::AssignmentList& list, std::uint64_t unixNow);
    // Writes a change, `now` being UI time: a change happens only when a match is
    // told, forgotten or kept longer, so there is no debounce. A failed write (the
    // file busy or not writable) keeps the change and is tried again a few seconds on.
    void Save(double now);
    static constexpr double RetrySeconds = 5;
private:
    std::vector<platform::AnnouncedMatch> told_;
    bool loaded_ = false, dirty_ = false;
    double retryAt_ = 0;
    Loader load_;
    Saver save_;
};
} }
