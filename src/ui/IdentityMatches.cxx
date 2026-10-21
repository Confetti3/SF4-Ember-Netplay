// The tournament matches screen: the matches a service assigned, which the
// player plays from here, the match a link named, and the reads that keep the
// list fresh (every minute away from the screen, and every few seconds after
// a match ended for a lost connection, when the bridge may set it up again).
#include "IdentityPanel.hxx"
#include "IdentityRows.hxx"
#include "../common/Localization.hxx"
#include "../common/TournamentLink.hxx"
#include <imgui.h>
#include <algorithm>

namespace sf4e { namespace ui {
using netplay::IdentityOp;
using netplay::IdentityRequest;
namespace {
// Away from the matches screen, the assigned matches are read this often.
constexpr double AssignmentsSeconds = 60;
// After a match ended for a lost connection, the bridge may set up a new one:
// the matches are read this often, from any screen, for this long.
constexpr double RestartPollSeconds = 5, RestartWatchSeconds = 180;
// How long after a fresh link its match may still play by itself.
constexpr double LinkPlaySeconds = 120;

bool Playing(const netplay::tournament::Status& t) { return netplay::tournament::PlayUnderWay(t.phase); }
std::string PlayState(const netplay::tournament::Status& t) {
    using netplay::tournament::Phase;
    switch (t.phase) {
    case Phase::Claiming: return loc::T("tournament.state.claiming");
    case Phase::Opening: return loc::T("tournament.state.opening");
    case Phase::InRoom:
        return t.waitingForPermit ? loc::T("tournament.state.waiting_permit") :
            t.waitingForOpponent ? loc::T("tournament.state.waiting_opponent") : loc::T("tournament.state.in_room");
    case Phase::Finished:
        return loc::T(t.reason == netplay::tournament::ConnectionLostReason ? "tournament.connection_lost_notice" : "tournament.state.finished");
    case Phase::Failed: return loc::T("tournament.state.failed");
    default: return {};
    }
}
}

void IdentityPanel::OpenMatch(const std::string& bridge, const std::string& match, bool playNow, std::uint64_t linkSequence) {
    bridge_ = bridge;
    wantAssignments_ = true;
    opened_ = OpenedMatch{bridge, "tm-play:" + match, std::nullopt, match};
    if (playNow) linkedPlay_ = LinkedPlay{match, now_ + LinkPlaySeconds, linkSequence};
    else linkedPlay_.reset();
    watchBridge_.clear(); watchMatch_.clear();
    // The fresh service list says whether the player trusts this one.
    IdentityRequest list; list.op = IdentityOp::BridgeList; Queue(std::move(list));
}

bool IdentityPanel::LinkMayPlay(const ShellView& v) {
    return v.session.room == netplay::RoomState::Idle && v.session.match == netplay::MatchState::None && !v.tournament.MatchRoomActive();
}

int IdentityPanel::ReadyMatches(const ShellView& v) const {
    const auto& id = v.identity;
    if (!id.known || id.state != "ready" || (servicesKnown_ && !FirstService(v))) return 0;
    const auto& list = v.tournament.list;
    if (list.bridge != bridge_) return 0;
    return static_cast<int>(std::count_if(list.items.begin(), list.items.end(),
        [&](const netplay::tournament::Assignment& match) { return match.Playable(v.unixNow); }));
}

bool IdentityPanel::TakeAssigned(const ShellView& v) {
    // A player waiting for the match set up again is told on the matches screen too.
    return announced_.Take(v.tournament.list, v.unixNow) && (lastScreen_ != "tournament-matches" || now_ < restartWatchUntil_);
}

std::string IdentityPanel::TakeFocus(const std::vector<MenuEntry>& rows) {
    if (!opened_ || std::none_of(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == opened_->row; })) return {};
    const std::string row = opened_->row;
    opened_.reset();
    return row;
}

void IdentityPanel::CheckOpenedMatch(const netplay::tournament::AssignmentList& list) {
    if (!opened_ || !opened_->finishedBefore || list.finished == *opened_->finishedBefore || list.bridge != opened_->bridge) return;
    const bool listed = std::any_of(list.items.begin(), list.items.end(),
        [&](const netplay::tournament::Assignment& item) { return "tm-play:" + item.matchId == opened_->row; });
    if (listed) return;
    // A failed refresh was said already. A match that is not the player's is theirs
    // to watch (the Watch row is offered), so for an organizer or a caster saying
    // so is not an error.
    if (!list.error.empty()) { opened_.reset(); return; }
    Say(loc::T("tournament.failure.not_yours"), Tone::Neutral, 12);
    watchBridge_ = opened_->bridge; watchMatch_ = opened_->match;
    opened_->row = "tm-watch:" + opened_->match;
}

bool IdentityPanel::CanPlay(const ShellView& v, const std::string& id) const {
    const auto& t = v.tournament;
    if (t.list.bridge != bridge_ || v.session.room != netplay::RoomState::Idle || t.MatchRoomActive()) return false;
    return std::any_of(t.list.items.begin(), t.list.items.end(),
        [&](const netplay::tournament::Assignment& match) { return match.matchId == id && match.Playable(v.unixNow); });
}

void IdentityPanel::SendTournament(const ShellView& v, const std::string& screen, const Submit& submit) {
    using netplay::tournament::Command;
    using netplay::tournament::Phase;
    const auto& t = v.tournament;
    if (loadingAssignments_ && !t.list.loading && t.list.error.empty()) ReadIfFirstUse(v, t.list.bridge);
    loadingAssignments_ = t.list.loading;
    CheckOpenedMatch(t.list);
    // A refresh's failure is said once; the runtime clears it when the next one is sent.
    if (t.list.error != assignmentsError_) {
        assignmentsError_ = t.list.error;
        if (!assignmentsError_.empty() && screen == "tournament-matches") Say(TournamentFailure(assignmentsError_), Tone::Error);
    }
    // A match that ended has a new score to show.
    if (t.phase != phase_) {
        if (t.phase == Phase::Finished || t.phase == Phase::Failed) wantAssignments_ = true;
        // One that ended for a lost connection may be set up again as a new
        // match on its service, which is watched for it from now on, until
        // the player plays a match again.
        if (t.phase == Phase::Finished && t.reason == netplay::tournament::ConnectionLostReason) {
            restartWatchUntil_ = now_ + RestartWatchSeconds; assignmentsAt_ = now_;
            if (FindBridge(v, t.bridgeId)) bridge_ = t.bridgeId;
        } else if (Playing(t)) restartWatchUntil_ = 0;
        phase_ = t.phase;
    }
    const bool watching = now_ < restartWatchUntil_;
    // A fresh link's match plays by itself once the list shows it playable,
    // with the checks Play has, while the player looks at the matches screen.
    if (linkedPlay_ && (linkedPlay_->linkSequence ? !t.link.direct || t.link.sequence != linkedPlay_->linkSequence : now_ > linkedPlay_->until))
        linkedPlay_.reset();
    if (!play_ && linkedPlay_ && screen == "tournament-matches" && v.identity.state == "ready" && FindBridge(v, bridge_) &&
        CanPlay(v, linkedPlay_->match)) {
        play_ = Command{Command::Op::Play, bridge_, linkedPlay_->match};
        linkedPlay_.reset();
        message_.clear();
    }
    // While a fresh link waits for its match, the list is read as often as after a lost connection.
    const bool linking = linkedPlay_.has_value();
    ShellAction action;
    action.command.generation = v.session.generation;
    if (play_) {
        action.tournament = std::move(*play_);
        play_.reset();
    } else if ((wantAssignments_ && screen == "tournament-matches" || (watching || linking || !Owns(screen)) && now_ >= assignmentsAt_ && servicesKnown_) &&
               FindBridge(v, bridge_) && !t.list.loading) {
        wantAssignments_ = false; assignmentsAt_ = now_ + (watching || linking ? RestartPollSeconds : AssignmentsSeconds);
        action.tournament.op = Command::Op::Refresh;
        action.tournament.bridgeId = bridge_;
        if (opened_ && opened_->bridge == bridge_) opened_->finishedBefore = t.list.finished;
    } else {
        return;
    }
    if (!submit(std::move(action))) Say(loc::T("error.queue_failed"), Tone::Error);
}

void IdentityPanel::MatchesRows(const ShellView& v, std::vector<MenuEntry>& rows) const {
    const bool ready = v.identity.state == "ready";
    if (!ready) {
        rows.push_back(InfoRow("id-linked-unavailable", loc::T("identity.linked_needs_id"), {}, loc::T("identity.linked_needs_id_detail")));
        // Connect Discord creates the Ember ID on the way.
        rows.push_back(Row(DiscordConnectScreen, loc::T("screen.connect_discord"), loc::T("identity.connect_discord_detail")));
        // A match link waits for one: the Ember ID screen makes it, and the match stays selected.
        if (opened_) rows.push_back(Row("identity", loc::T("screen.identity"), loc::T("tournament.watch.needs_id")));
        return;
    }
    const auto& t = v.tournament;
    const bool playing = Playing(t);
    if (!t.matchId.empty() && t.phase != netplay::tournament::Phase::Idle) {
        const bool failed = t.phase == netplay::tournament::Phase::Failed;
        rows.push_back(InfoRow("tm-current", loc::T("tournament.current"), PlayState(t),
            failed ? TournamentFailure(t.reason) : std::string(loc::T("tournament.current_detail"))));
        if (playing) rows.push_back(ConfirmRow("tm-stop", loc::T("tournament.stop"), loc::T("tournament.stop_detail")));
    }
    AppendWatchState(v, rows);
    const auto* bridge = FindBridge(v, bridge_);
    if (!bridge) {
        rows.push_back(InfoRow("tm-no-service", loc::T("tournament.needs_service"), {}, loc::T("tournament.needs_discord_detail")));
        // Connect Discord sets up Ember's own service; Linked accounts any other.
        rows.push_back(Row(DiscordConnectScreen, loc::T("screen.connect_discord"), loc::T("identity.connect_discord_detail")));
        rows.push_back(Row("linked-accounts", loc::T("screen.linked_accounts"), loc::T("identity.linked_detail")));
        // A match link to a service that is not trusted yet cannot be watched until it is.
        if (const auto watch = WatchEntry(v)) rows.push_back(*watch);
        return;
    }
    rows.push_back(ServiceRow(v, *bridge));
    if (t.list.bridge == bridge_) {
        const bool free = v.session.room == netplay::RoomState::Idle && !t.MatchRoomActive();
        for (const auto& match : t.list.items) {
            const bool slotKnown = match.slot == 0 || match.slot == 1;
            const auto own = slotKnown ? match.wins[static_cast<std::size_t>(match.slot)] : 0u;
            const auto other = slotKnown ? match.wins[static_cast<std::size_t>(1 - match.slot)] : 0u;
            std::string detail = loc::Tf("tournament.row_detail",
                match.opponentFingerprint.empty() ? std::string("?") : match.opponentFingerprint, match.slot + 1);
            detail += "\n\n";
            // Expired by the bridge's word, or by this PC's clock while it waits for its players; a
            // match running or in review keeps the bridge's state and has no Play until it waits again.
            const bool expired = match.ExpiredBy(v.unixNow);
            const bool playable = match.Playable(v.unixNow);
            detail += expired ? loc::T("tournament.expired_detail") : match.Finished() ? loc::T("tournament.finished_detail") :
                !match.PlayedInEmber() ? loc::T("tournament.organizer_detail") : !playable ? loc::T("tournament.state.waiting_permit") :
                free ? loc::T("tournament.play_detail") : loc::T("tournament.leave_room_detail");
            // A match the player can play now reads as the button it is: Play, with its round.
            const bool canPlay = playable && free;
            const std::string label = canPlay ? (match.roundLabel.empty() ? std::string(loc::T("tournament.play")) : loc::Tf("tournament.play_round", match.roundLabel)) :
                match.roundLabel.empty() ? std::string(loc::T("tournament.match")) : match.roundLabel;
            auto row = Row("tm-play:" + match.matchId, label, std::move(detail), canPlay);
            row.value = expired ? std::string(loc::T("tournament.expired")) : loc::Tf("tournament.score", own, other, match.gamesToWin);
            row.userText = !match.roundLabel.empty();
            row.hint = loc::T("tournament.play");
            if (!playable) row.info = true;
            rows.push_back(std::move(row));
        }
        if (const auto watch = WatchEntry(v)) rows.push_back(*watch);
        if (t.list.items.empty() && !t.list.loading && t.list.error.empty())
            rows.push_back(InfoRow("tm-none", loc::T("tournament.none"), {}, loc::T("tournament.none_detail")));
    }
    rows.push_back(Row("tm-refresh", loc::T("identity.refresh"), loc::T("tournament.refresh_detail"), !t.list.loading));
    rows.push_back(Row("tm-paste", loc::T("tournament.paste_link"), loc::T("tournament.paste_link_detail")));
    rows.back().hint = loc::T("menu.hint.paste");
}

bool IdentityPanel::MatchesActivate(const MenuAction& a, const ShellView& v) {
    if (a.id == "tm-refresh") { wantAssignments_ = true; message_.clear(); return true; }
    if (a.id == "tm-paste") {
        // The match link the tournament site gave: its page or the ember: link.
        const char* text = ImGui::GetClipboardText();
        const auto link = tournament_link::ParsePasted(text ? text : "");
        if (!link.Valid()) { Say(loc::T("tournament.failure.link"), Tone::Error); return true; }
        message_.clear();
        OpenMatch(link.bridgeId, link.matchId, LinkMayPlay(v));
        return true;
    }
    if (a.id == "tm-stop") { play_ = netplay::tournament::Command{netplay::tournament::Command::Op::Stop, {}, {}}; return true; }
    if (ActivateWatch(a, v)) return true;
    if (a.id.compare(0, 8, "tm-play:") == 0) {
        // The row may be older than the list: the match is played only if it still can be.
        if (!CanPlay(v, a.id.substr(8))) return true;
        play_ = netplay::tournament::Command{netplay::tournament::Command::Op::Play, bridge_, a.id.substr(8)};
        message_.clear();
        return true;
    }
    return false;
}
} }
