#pragma once
// Room chat over a fight (docs/design/FIGHT_CHAT.md): the newest few messages
// said since the fight began, each shown for a few seconds and then fading,
// with a member's repeated line folded into one ("GG x2"). It reads the
// transcript the shell keeps whether or not Ember is shown, and owns only when
// each message first arrived. Drawn by the overlay's layers, never over Ember's
// own windows.
#include "ChatTranscript.hxx"
#include "MenuNavigation.hxx"
#include "Theme.hxx"
#include "../netplay/PlayerPreferences.hxx"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct FightChatEntry {
    std::string name, text;
    unsigned repeats = 1;
    bool own = false;
    // 1 while it stands, down to 0 over its fade.
    float alpha = 1;
    // Who said it: a repeat folds only into the same member's line.
    room::MemberId sender = 0;
};
struct FightChatFeedView {
    std::vector<FightChatEntry> entries;
};

// Who the feed listens to: everyone, or the members at one table (its fighters,
// queue and watchers) while this player has a place there.
struct FightChatAudience {
    bool everyone = true;
    std::set<room::MemberId> members;
    bool Hears(room::MemberId sender) const { return everyone || members.count(sender) > 0; }
};
// The audience `scope` gives in `room`: the table this PC's member fights at,
// queues for or watches. With no such table MyTable hears the whole room, as a
// fight of this PC's is always at a table it has a place at.
FightChatAudience AudienceOf(const room::Snapshot& room, netplay::FightChatPreferences::Scope scope);

class FightChatFeed {
public:
    static constexpr std::size_t MaximumShown = 3;
    static constexpr double ShownSeconds = 6, FadeSeconds = 1;
    // Every frame. `fighting`: a fight this PC plays or watches is on. Each fight
    // starts after the transcript's newest message, so nothing said before it
    // shows over it; a message's time is when this feed first saw it, and each
    // frame moves the mark to the newest message, so a line is stamped once.
    void Update(const ChatTranscript& transcript, bool fighting, double now);
    // What to draw now: none outside a fight or with Off; a muted member's lines
    // never; each message's text as the Chat screen shows it (ChatLineText).
    FightChatFeedView View(const ChatTranscript& transcript, const std::set<room::MemberId>& muted, const FightChatAudience& audience,
        bool filtered, double now) const;
private:
    bool fighting_ = false;
    // The newest message sequence the last Update took in: only lines above it are new.
    std::uint64_t seen_ = 0;
    std::map<std::uint64_t, double> arrived_;
};

// `strip`: where the match strip's panel is drawn (not valid when there is none).
// The feed takes the left, under the life bar, unless the panel is there; then the right.
void DrawFightChatFeed(const FightChatFeedView& view, const MatchStripBox& strip);

// Settings' fight chat rows: Chat in matches, Quick lines and the line of each
// of the four quick slots. They sit with the room chat's own settings, which a
// room leaves open.
std::vector<MenuEntry> FightChatRows(const netplay::FightChatPreferences& chat, bool enabled);
// Left or Right on one of those rows; false for any other row.
bool AdjustFightChat(const std::string& id, int delta, netplay::FightChatPreferences& chat);
} }
