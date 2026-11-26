// Fight chat's part of the shell (docs/design/FIGHT_CHAT.md): sending a quick
// line, and what room chat shows over a fight. The feed runs on every frame the
// overlay draws, whether Ember is shown or not; quick lines only on a frame Ember
// is hidden (the overlay drops them while it is shown).
#include "ApplicationShell.hxx"
#include "RoomFeedback.hxx"

namespace sf4e { namespace ui {
namespace {
// The room takes one chat line a second from each member (RoomAuthority::ApplyChat).
constexpr double QuickLineSeconds = 1;
}

// A quick line goes now or not at all: one the room cannot take at this moment,
// or a second inside the room's one a second, is dropped rather than sent after
// the moment it was for. Whether quick lines are on was decided by the pad update
// that asked for them (overlay::QuickLinesLive); only the room's rules apply here.
void ApplicationShell::SendQuickLines(unsigned slots, const ShellView& v, const Submit& submit) {
    if (!slots || ChatGateOf(v) != ChatGate::Writable) return;
    for (int slot = 0; slot < quickchat::Slots; ++slot) {
        if (!(slots & (1u << slot))) continue;
        const double now = clock_.Now();
        if (now - lastQuickLineAt_ < QuickLineSeconds) return;
        const QuickLine line = preferences_.fightChat.slots[slot];
        room::Action request;
        request.table = static_cast<std::uint8_t>(selectedTable_);
        request.kind = room::ActionKind::Chat;
        request.quick = line;
        request.text = quickchat::RowOf(line).english;
        if (SendRoom(std::move(request), v, submit)) lastQuickLineAt_ = now;
    }
}

FightChatFeedView ApplicationShell::FightFeed(const room::Snapshot& room, bool fighting) {
    const auto& chat = preferences_.fightChat;
    const double now = clock_.Now();
    const bool shown = fighting && chat.scope != netplay::FightChatPreferences::Scope::Off;
    fightFeed_.Update(transcript_, shown, now);
    // The audience is worked out only for a fight that shows the feed.
    if (!shown) return {};
    return fightFeed_.View(transcript_, muted_, AudienceOf(room, chat.scope), preferences_.chatFilter, now);
}
} }
