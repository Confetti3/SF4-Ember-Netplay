#include "RoomControls.hxx"
#include "RoomFeedback.hxx"
#include "Theme.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

namespace sf4e { namespace ui { namespace room_controls {
namespace {
const char* ChangeBlockerKey(room::SeatHold hold) {
    switch (hold) {
    case room::SeatHold::UnreadyFirst: return "room.change_fighter.unready";
    case room::SeatHold::Unresolved: return "room.result_unresolved.detail";
    case room::SeatHold::AwaitingResult: return "room.awaiting_result";
    default: return "room.match_active";
    }
}
constexpr SeatOption SeatOptionList[] = {
    {"sit-p1", room::ActionKind::Queue, 0, "room.take_p1_seat", "room.take_p1_seat"},
    {"sit-p2", room::ActionKind::Queue, 1, "room.take_p2_seat", "room.take_p2_seat"},
    {"queue", room::ActionKind::Queue, -1, "room.join_queue", "room.join_queue"},
    {"watch", room::ActionKind::Watch, -1, "room.choice.watch", "room.watch_next"},
    {"unwatch", room::ActionKind::Unwatch, -1, "room.choice.unwatch", "room.stop_watching"},
};
// Why a seated fighter cannot ready at this table now, or empty. Everything
// the room is still finishing (draining, checkpoint, receipt, result) stays
// behind the runtime, which parks the press and reports only a real failure.
std::string ReadyBlocker(const ShellView& v, const room::Table& t) {
    using room::TablePhase;
    const bool finishedGame = t.phase == TablePhase::Playing && v.session.match == netplay::MatchState::PostMatch;
    if (!RoomActionsAvailable(v) && !RoomCheckpointPending(v)) return RoomWaitReason(v);
    if (t.phase == TablePhase::Paused) return loc::T("room.result_unresolved.detail");
    if (t.phase == TablePhase::Ready) return loc::T("room.both_ready");
    if (t.phase == TablePhase::Playing && !finishedGame) return loc::T("room.match_in_progress.detail");
    if (!t.p1 || !t.p2) return loc::T("room.waiting_other_seat");
    if (!v.controllerReady) return loc::T("room.controller_required");
    return v.selectionError;
}
}
bool GameLive(const ShellView& v) {
    return v.session.match == netplay::MatchState::Preparing || v.session.match == netplay::MatchState::Playing;
}
bool PostMatch(const ShellView& v) { return v.session.match == netplay::MatchState::PostMatch; }
const char* LeaveBlockerKey(room::SeatHold hold) {
    switch (hold) {
    case room::SeatHold::UnreadyFirst: return "room.leave_seat.unready_first";
    case room::SeatHold::Unresolved: return "room.leave_seat.abandon_first";
    case room::SeatHold::AwaitingResult: return "room.awaiting_result";
    default: return "room.leave_seat.finish_first";
    }
}
bool TerminalFenced(const room::Snapshot& snapshot, room::ActionKind kind, std::size_t table) {
    if (kind != room::ActionKind::Queue && kind != room::ActionKind::Watch &&
        kind != room::ActionKind::Ready && kind != room::ActionKind::Unready) return false;
    return snapshot.localTerminalPending ||
        (table < snapshot.terminalPending.size() && snapshot.terminalPending[table]);
}
const char* TerminalPendingReason() {
    return loc::T("room.terminal_pending");
}
std::string LeaveRoomDetail(const ShellView& v) {
    std::string detail = loc::T("room.leave.detail");
    const auto* local = room::FindMember(v.room, v.room.localMember);
    const auto place = room::PlaceOf(v.room, v.room.localMember);
    if (place.kind == room::Place::Kind::Seat) {
        const auto& table = v.room.tables[place.table];
        if (table.phase == room::TablePhase::Paused) detail += loc::T("room.leave.unresolved");
        else if (table.phase == room::TablePhase::Playing || (table.phase == room::TablePhase::Ready && !table.spectatorHold))
            detail += loc::T("room.leave.active");
        else detail += loc::T("room.leave.seat");
    } else if (place.kind == room::Place::Kind::Queue) detail += loc::T("room.leave.queue");
    if (local && local->host && v.room.members.size() > 1) detail += loc::T("room.leave.host");
    return detail;
}
const SeatOption* FindSeatOption(const std::string& id) {
    for (const auto& option : SeatOptionList) if (id == option.id) return &option;
    return nullptr;
}
std::vector<MenuChoice> SeatChoices(const ShellView& v, const room::Table& t) {
    const bool open0 = room::SeatOpenNow(t, 0), open1 = room::SeatOpenNow(t, 1);
    const char* left = open0 ? "sit-p1" : open1 ? "sit-p2" : "queue";
    const char* right = open0 && open1 ? "sit-p2" : room::WatchesByChoice(t, v.room.localMember) ? "unwatch" : "watch";
    std::vector<MenuChoice> choices;
    for (const char* id : {left, right}) {
        const auto* option = FindSeatOption(id);
        MenuChoice choice{id, loc::T(option->label), loc::T(option->detail)};
        if (!RoomActionsAvailable(v)) { choice.enabled = false; choice.detail = RoomWaitReason(v); }
        else if (TerminalFenced(v.room, option->kind, t.id)) { choice.enabled = false; choice.detail = TerminalPendingReason(); }
        choices.push_back(std::move(choice));
    }
    choices.push_back({"options", loc::T("room.legend_options"), loc::T("room.table_options")});
    return choices;
}
PlaceExit ExitFromPlace(const ShellView& v) {
    PlaceExit exit;
    const auto place = room::PlaceOf(v.room, v.room.localMember);
    if (place.kind == room::Place::Kind::Seat) {
        const auto& t = v.room.tables[place.table];
        exit.table = place.table; exit.seat = true; exit.label = loc::T("room.leave_seat");
        const auto hold = room::LeavingSeatHold(t, place.seat, GameLive(v), PostMatch(v));
        if (hold == room::SeatHold::None) { exit.allowed = true; exit.cost = room::CostOfLeavingSeat(t); }
        else if (hold == room::SeatHold::UnreadyFirst) {
            exit.allowed = true; exit.unready = true; exit.seatIndex = place.seat; exit.label = loc::T("room.cancel_start");
        }
        else exit.blocker = loc::T(LeaveBlockerKey(hold));
        // A tournament fighter's seat is the match's: it is given up only by
        // leaving the room, so there is nothing to leave here but a held start.
        if (v.room.tournament.Active() && place.table == room::TournamentTable && !exit.unready) return PlaceExit{};
    } else if (place.kind == room::Place::Kind::Queue) {
        exit.table = place.table; exit.allowed = true; exit.label = loc::T("room.leave_queue");
    }
    return exit;
}
bool OnOwnPlace(const PlaceExit& place, const std::string& focus) {
    return place.table >= 0 && focus == "table-" + std::to_string(place.table);
}
bool LeaveBlocked(const ShellView& v) {
    const auto exit = ExitFromPlace(v);
    return exit.table >= 0 && !exit.allowed;
}
std::string LeavingCostText(const room::LeavingCost& cost, const room::Table& t) {
    std::string text;
    if (cost.score) text = loc::Tf("room.leave_seat.costs_score", SetScoreText(t.score));
    if (cost.handsOver) text += std::string(text.empty() ? "" : " ") + loc::T("room.leave_seat.hands_over");
    return text;
}
ReadyControl DescribeReady(const ShellView& v, const room::Table& t, int seat) {
    using room::TablePhase;
    ReadyControl control;
    if (seat >= 0 && seat < 2 && room::ReadyCancellable(t, seat)) {
        control.label = loc::T("room.unready");
        control.detail = t.spectatorHold ? HoldText(v, t) + " " + loc::T("room.hold.cancel_detail") : std::string(loc::T("room.ready.cancel_detail"));
        if (RoomActionsAvailable(v)) control.kind = ReadyControl::Unready;
        else control.refusal = RoomWaitReason(v);
        return control;
    }
    // An in-flight Ready already says so on the control, so a second press
    // needs no refusal of its own.
    if (v.readyRequested) { control.label = loc::T("room.readying"); control.detail = loc::T("room.ready.locking"); return control; }
    const bool postMatch = v.session.match == netplay::MatchState::PostMatch;
    const bool finishedGame = t.phase == TablePhase::Playing && postMatch;
    control.label = loc::T(t.phase == TablePhase::Paused ? "room.phase.unresolved" : t.phase == TablePhase::Ready ? "room.preparing_match" :
        t.phase == TablePhase::Playing && !finishedGame ? "room.match_in_progress" : postMatch ? "room.ready_rematch" : "room.ready_up");
    control.detail = control.refusal = ReadyBlocker(v, t);
    if (control.detail.empty()) {
        control.kind = postMatch ? ReadyControl::Rematch : ReadyControl::Ready;
        control.detail = std::string(loc::T("room.ready.lock_detail")) + "\n" + v.selectionSummary;
        if (v.opponentChangedFighter >= 0) control.detail = OpponentChangedText(v.opponentChangedFighter) + "\n" + control.detail;
    }
    return control;
}
std::string OpponentChangedText(int fighter) {
    const auto* found = selection::FindFighter(fighter);
    return loc::Tf("room.opponent_changed_fighter", found ? found->name : "?");
}
bool HoldingStart(const ShellView& v, const room::Table& t) {
    const auto* local = room::FindMember(v.room, v.room.localMember);
    return t.spectatorHold && local && local->spectatorLocked && v.room.localTerminalPending &&
        std::find(t.spectators.begin(), t.spectators.end(), local->id) != t.spectators.end();
}
std::string HoldText(const ShellView& v, const room::Table& t) {
    if (!t.spectatorHold || t.phase != room::TablePhase::Ready) return {};
    // An older host sends no time left: say how long it can last.
    if (!t.holdRemainingMs) return loc::Tf("room.waiting_spectators", room::SpectatorStartHoldMs / 1000);
    const unsigned seconds = (std::max)(1u, (t.holdRemainingMs + 999) / 1000);
    if (HoldingStart(v, t)) return loc::Tf("room.hold.waiting_you", seconds);
    // The room knows which locked-in spectators are still returning; the
    // snapshot names who is locked in, which is who the start can wait for.
    std::string names;
    for (const auto id : t.spectators) {
        const auto* member = room::FindMember(v.room, id);
        if (!member || !member->spectatorLocked || id == v.room.localMember) continue;
        names += (names.empty() ? "" : ", ") + member->name;
    }
    return names.empty() ? loc::Tf("room.hold.waiting_some", seconds) : loc::Tf("room.hold.waiting_for", names, seconds);
}
std::string HoldStatus(const ShellView& v, const room::Table& t) {
    if (!t.spectatorHold || t.phase != room::TablePhase::Ready) return {};
    if (!t.holdRemainingMs) return loc::T("room.phase.holding");
    const unsigned seconds = (std::max)(1u, (t.holdRemainingMs + 999) / 1000);
    return HoldingStart(v, t) ? loc::Tf("room.hold.waiting_you", seconds) : loc::Tf("room.hold.status", seconds);
}
TableBanner DescribeTableBanner(const ShellView& v, const room::Table& t) {
    TableBanner banner;
    banner.text = HoldText(v, t);
    if (!banner.text.empty()) return banner;
    const auto place = room::PlaceOf(v.room, v.room.localMember);
    // OpponentFighterWatch keeps the fighter only while the local player sits
    // at this matchup unready, so the seat is the other one at their table.
    if (v.opponentChangedFighter >= 0 && place.kind == room::Place::Kind::Seat && place.table == static_cast<int>(t.id)) {
        banner.text = OpponentChangedText(v.opponentChangedFighter);
        banner.seat = 1 - place.seat;
    }
    return banner;
}
std::string SelectionBlocker(const ShellView& v) {
    using room::TablePhase;
    const auto& s = v.room;
    if (s.localTerminalPending) return TerminalPendingReason();
    const bool mutableRoom = RoomActionsAvailable(v);
    if (mutableRoom && v.canEditSelection) return {};
    const auto place = room::PlaceOf(s, s.localMember);
    if (place.kind == room::Place::Kind::Seat) {
        const auto& t = s.tables[place.table];
        const bool active = t.phase == TablePhase::Ready || t.phase == TablePhase::Playing || t.phase == TablePhase::Paused;
        const auto hold = room::ChangingFighterHold(t, place.seat, PostMatch(v));
        if (active && !mutableRoom && !RoomCheckpointPending(v)) return RoomWaitReason(v);
        if (hold == room::SeatHold::UnreadyFirst) return loc::T(ChangeBlockerKey(hold));
        if (active && TerminalFenced(s, room::ActionKind::Ready, place.table)) return TerminalPendingReason();
        if (hold != room::SeatHold::None) return loc::T(ChangeBlockerKey(hold));
    }
    if (!mutableRoom) return RoomWaitReason(v);
    return !v.selectionLockReason.empty() ? v.selectionLockReason : std::string(loc::T("room.change_fighter.waiting"));
}
} } }
