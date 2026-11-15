#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sf4e { namespace ui {

// Every reason the game opens Ember's menu by itself, each with its own
// memory of what it already opened the menu for. The reasons are checked in
// this order, once per frame.
enum class SummonsReason : unsigned {
    MatchResult, DiscordInvite, JoinLink, TrainingCall, DiscordConnectLink, ReplayLink, PublicRoomLink, Count };

constexpr const char* SummonsName(SummonsReason reason) {
    return reason == SummonsReason::MatchResult ? "the match result" :
        reason == SummonsReason::DiscordInvite ? "a Discord invite" :
        reason == SummonsReason::JoinLink ? "a room link" :
        reason == SummonsReason::TrainingCall ? "a Training call" :
        reason == SummonsReason::DiscordConnectLink ? "a Discord connect link" :
        reason == SummonsReason::ReplayLink ? "a replay link" : "a public room link";
}

// The key of an event counted by a sequence number: 0 is no event yet.
inline std::string SummonsSequenceKey(std::uint64_t sequence) { return sequence ? std::to_string(sequence) : std::string(); }

// What one reason says this frame.
struct SummonsAsk {
    // Which event it is: a sequence number or the link itself. Empty when the
    // reason has nothing now, which also forgets what it last opened the menu
    // for, so the same event coming again opens it again.
    std::string key;
    // Whether the menu may open for it now. A key that is not ready waits.
    bool ready = false;
    // Whether the event, once ready, opens the menu or is only taken as seen.
    bool opens = true;
};

class MenuSummons {
public:
    // Calls open(name) for each reason with a new key that is ready, and
    // remembers the key: a reason opens the menu once per event.
    template<class Open>
    void Update(const std::array<SummonsAsk, static_cast<std::size_t>(SummonsReason::Count)>& asks, const Open& open) {
        for (std::size_t i = 0; i < asks.size(); ++i) {
            const auto& ask = asks[i];
            auto& shown = shown_[i];
            if (ask.key.empty()) { shown.clear(); continue; }
            if (ask.key == shown || !ask.ready) continue;
            if (ask.opens) open(SummonsName(static_cast<SummonsReason>(i)));
            shown = ask.key;
        }
    }
private:
    std::array<std::string, static_cast<std::size_t>(SummonsReason::Count)> shown_;
};

} }
