#include "MatchAnnouncements.hxx"
#include <algorithm>
#include <string>

namespace sf4e { namespace ui {
namespace {
using netplay::tournament::Assignment;
// An entry is kept this long past the last list of its service that showed it, so a
// service that is down, refused or not visited for a while keeps what it was told.
constexpr std::uint64_t RetainSeconds = 30ull * 24 * 3600;
// The time is moved on once a day at most, so a file is not written at every list.
constexpr std::uint64_t RenewSeconds = 24ull * 3600;
}

bool MatchAnnouncements::Take(const netplay::tournament::AssignmentList& list, std::uint64_t now) {
    if (!loaded_) { told_ = load_ ? load_() : platform::LoadAnnouncedMatches(); loaded_ = true; }
    const auto listed = [&](const std::string& id) {
        return std::any_of(list.items.begin(), list.items.end(), [&](const Assignment& item) { return item.matchId == id; });
    };
    // A list read just now is its service's word on that service's entries, the only
    // one that forgets them: those it no longer lists go, and those it lists are kept
    // a month from now, whatever their state or expiry says (a new game can move it).
    // A list loading or failed, or another service's, says nothing about them.
    const bool read = !list.bridge.empty() && !list.loading && list.error.empty() && list.finished > 0;
    const auto before = told_.size();
    if (read) {
        for (auto it = told_.begin(); it != told_.end();) {
            if (it->bridge != list.bridge) { ++it; continue; }
            if (!listed(it->id)) { it = told_.erase(it); continue; }
            if (now && it->until < now + RetainSeconds - RenewSeconds) { it->until = now + RetainSeconds; dirty_ = true; }
            ++it;
        }
    }
    // A service not heard from for a month: its entries go, the one rule by time.
    told_.erase(std::remove_if(told_.begin(), told_.end(), [&](const platform::AnnouncedMatch& told) {
        return now && told.until && now > told.until;
    }), told_.end());
    bool fresh = false;
    for (const auto& match : list.items) {
        const bool known = std::any_of(told_.begin(), told_.end(), [&](const platform::AnnouncedMatch& told) {
            return told.bridge == list.bridge && told.id == match.matchId;
        });
        if (known || !match.Playable(now)) continue;
        told_.push_back({list.bridge, match.matchId, now ? now + RetainSeconds : 0});
        fresh = true;
    }
    // Within the bounds, this service's entries first to stay: it lists at most 50,
    // so its live matches always fit, and another service's go before it does.
    if (fresh) platform::TrimAnnouncedMatches(told_, list.bridge);
    if (fresh || told_.size() != before) dirty_ = true;
    return fresh;
}

void MatchAnnouncements::Save(double now) {
    if (!dirty_ || now < retryAt_) return;
    std::string diagnostic;
    const bool saved = save_ ? save_(told_) : platform::SaveAnnouncedMatches(told_, diagnostic);
    if (saved) dirty_ = false; else retryAt_ = now + RetrySeconds;
}
} }
