// Watching a tournament match from the matches screen (match watch): the
// Watch row a link to a match that is not the player's offers, the line that
// says how the watch is going, and Watch itself.
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
namespace {
// What watching a match is doing, in words: the phase line, the bridge's
// refusal, or who plays when the room is joined.
std::string WatchState(const netplay::tournament::Status& t, const room::Snapshot& room) {
    using netplay::tournament::WatchPhase;
    switch (t.watch.phase) {
    case WatchPhase::Asking: return loc::T("tournament.watch.state.asking");
    case WatchPhase::Waiting: return loc::T(t.watch.full ? "tournament.watch.state.full" : "tournament.watch.state.waiting_room");
    case WatchPhase::Joining: return loc::T("tournament.watch.state.joining");
    case WatchPhase::Watching: {
        const auto& table = room.tables[room::TournamentTable];
        const auto name = [&](room::MemberId id) {
            const auto* member = room::FindMember(room, id);
            return member ? member->name : std::string(loc::T("room.player"));
        };
        return loc::Tf("tournament.watch.state.watching", name(table.p1), name(table.p2));
    }
    case WatchPhase::Finished: return loc::T("tournament.watch.finished");
    case WatchPhase::Refused: {
        const auto& code = t.watch.reason;
        if (code == "not_allowed") return loc::T("tournament.watch.refused.not_allowed");
        if (code == "revoked") return loc::T("tournament.watch.refused.revoked");
        if (code == "fighter") return loc::T("tournament.watch.refused.fighter");
        if (code == "incompatible_build") return loc::T("tournament.watch.refused.build");
        if (code == "watch_unsupported") return loc::T("tournament.watch.refused.unsupported");
        if (code == "other_room") return loc::T("tournament.watch.refused.other_room");
        return IdentityPanel::TournamentFailure(code);
    }
    default: return {};
    }
}
}

bool IdentityPanel::CanWatch(const ShellView& v) const {
    const auto& t = v.tournament;
    return !watchMatch_.empty() && watchBridge_ == bridge_ && v.identity.state == "ready" && FindBridge(v, bridge_) &&
        v.session.room == netplay::RoomState::Idle && !t.MatchRoomActive();
}

std::optional<MenuEntry> IdentityPanel::WatchEntry(const ShellView& v) const {
    if (watchMatch_.empty()) return std::nullopt;
    const std::string id = "tm-watch:" + watchMatch_;
    // A service the player does not trust cannot be asked; it is shown wherever the player is looking.
    if (!FindBridge(v, watchBridge_)) return InfoRow(id, loc::T("tournament.watch.row"), {}, loc::T("tournament.watch.untrusted_bridge"));
    if (watchBridge_ != bridge_) return std::nullopt;
    auto watch = Row(id, loc::T("tournament.watch.row"), loc::T("tournament.watch.row_detail"), CanWatch(v));
    watch.hint = loc::T("room.choice.watch");
    return watch;
}

// The line for a watch under way or just ended, with Stop while it is under way.
void IdentityPanel::AppendWatchState(const ShellView& v, std::vector<MenuEntry>& rows) const {
    const auto& t = v.tournament;
    if (t.watch.matchId.empty() || t.watch.phase == netplay::tournament::WatchPhase::Idle) return;
    rows.push_back(InfoRow("tm-watching", loc::T("tournament.current"), WatchState(t, v.room), loc::T("tournament.watch.row_detail")));
    if (netplay::tournament::WatchUnderWay(t.watch.phase))
        rows.push_back(ConfirmRow("tm-stop", loc::T("room.stop_watching"), loc::T("tournament.watch.stop_detail")));
}

// Watch on the Watch row. True when `action` was one.
bool IdentityPanel::ActivateWatch(const MenuAction& action, const ShellView& v) {
    if (action.id.compare(0, 9, "tm-watch:") != 0) return false;
    // The row may be older than the screen: a match is watched only if it still can be.
    if (!CanWatch(v) || action.id.substr(9) != watchMatch_) return true;
    play_ = netplay::tournament::Command{netplay::tournament::Command::Op::Watch, bridge_, watchMatch_};
    message_.clear();
    return true;
}

} }
