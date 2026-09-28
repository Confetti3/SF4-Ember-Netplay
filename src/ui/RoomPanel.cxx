#include "ApplicationShell.hxx"
#include "Theme.hxx"
#include "MenuRows.hxx"
#include "RoomFeedback.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <utility>

namespace sf4e { namespace ui {
namespace {
const room::Member* Member(const room::Snapshot& snapshot, room::MemberId id) {
    const auto found = std::find_if(snapshot.members.begin(), snapshot.members.end(),
        [id](const room::Member& member) { return member.id == id; });
    return found == snapshot.members.end() ? nullptr : &*found;
}
// The durable receipt fence gates admission to a table, never departure from
// it: the member the room is waiting on can be exactly the one that needs to
// leave. RoomAuthority accepts Unqueue and Unwatch while a receipt is
// outstanding, so neither is fenced here.
bool TerminalFenced(const room::Snapshot& snapshot, room::ActionKind kind, std::size_t table) {
    if (kind != room::ActionKind::Queue && kind != room::ActionKind::Watch &&
        kind != room::ActionKind::Ready && kind != room::ActionKind::Unready) return false;
    return snapshot.localTerminalPending ||
        (table < snapshot.terminalPending.size() && snapshot.terminalPending[table]);
}
const char* Name(const room::Snapshot& snapshot, room::MemberId id) {
    const auto* member = Member(snapshot, id);
    return member ? member->name.c_str() : loc::T(id ? "room.member_left" : "room.open_seat");
}
const char* StatusName(room::MemberStatus status) {
    switch (status) {
    case room::MemberStatus::Queued: return loc::T("room.status.queued");
    case room::MemberStatus::Seated: return loc::T("room.status.seated");
    case room::MemberStatus::Ready: return loc::T("room.status.ready");
    case room::MemberStatus::Playing: return loc::T("room.status.playing");
    case room::MemberStatus::Watching: return loc::T("room.status.watching");
    case room::MemberStatus::WatchingNext: return loc::T("room.status.watching_next");
    default: return loc::T("room.status.idle");
    }
}
const char* PhaseName(room::TablePhase phase) {
    switch (phase) {
    case room::TablePhase::Waiting: return loc::T("room.phase.waiting");
    case room::TablePhase::Ready: return loc::T("room.phase.preparing");
    case room::TablePhase::Playing: return loc::T("room.phase.in_game");
    case room::TablePhase::Paused: return loc::T("room.phase.unresolved");
    case room::TablePhase::Closed: return loc::T("room.phase.closed");
    default: return loc::T("room.phase.open");
    }
}
const char* TerminalPendingReason() {
    return loc::T("room.terminal_pending");
}
// One description of Leave room for both screens, stating what it costs.
std::string LeaveRoomDetail(const ShellView& v) {
    std::string detail = loc::T("room.leave.detail");
    const auto* local = Member(v.room, v.room.localMember);
    if (local && local->table >= 0 && local->table < static_cast<std::int8_t>(room::TableCount)) {
        const auto& table = v.room.tables[local->table];
        const bool seated = local->seat >= 0 && local->seat < 2;
        if (seated && table.phase == room::TablePhase::Paused) detail += loc::T("room.leave.unresolved");
        else if (seated && (table.phase == room::TablePhase::Playing || table.phase == room::TablePhase::Ready)) detail += loc::T("room.leave.active");
        else if (seated) detail += loc::T("room.leave.seat");
        else if (local->status == room::MemberStatus::Queued) detail += loc::T("room.leave.queue");
    }
    if (local && local->host && v.room.members.size() > 1) detail += loc::T("room.leave.host");
    return detail;
}
// The seat chooser's options. A choice carries the option's id, so dispatch
// never re-derives what the player picked from a newer snapshot.
// Each has a button label short enough for three to a card, and its full
// wording.
struct SeatOption { const char* id; room::ActionKind kind; int seat; const char* label; const char* detail; };
constexpr SeatOption SeatOptionList[] = {
    {"sit-p1", room::ActionKind::Queue, 0, "room.take_p1_seat", "room.take_p1_seat"},
    {"sit-p2", room::ActionKind::Queue, 1, "room.take_p2_seat", "room.take_p2_seat"},
    {"queue", room::ActionKind::Queue, -1, "room.join_queue", "room.join_queue"},
    {"watch", room::ActionKind::Watch, -1, "room.choice.watch", "room.watch_next"},
    {"unwatch", room::ActionKind::Unwatch, -1, "room.choice.unwatch", "room.stop_watching"},
};
const SeatOption* FindSeatOption(const std::string& id) {
    for (const auto& option : SeatOptionList) if (id == option.id) return &option;
    return nullptr;
}
// Left to right: both seats when the table is empty, otherwise play (the
// open seat, or the queue) and watch; then the table's options, so looking
// at a table never means joining it.
// A sit option is offered only where the authority would grant that seat now:
// open, between games, with nobody queued first.
std::vector<MenuChoice> SeatChoices(const room::Table& t, room::MemberId member) {
    const bool takable = t.phase != room::TablePhase::Playing && t.phase != room::TablePhase::Paused && t.queue.empty();
    const char* left = takable && !t.p1 ? "sit-p1" : takable && !t.p2 ? "sit-p2" : "queue";
    const char* right = takable && !t.p1 && !t.p2 ? "sit-p2" : room::WatchesByChoice(t, member) ? "unwatch" : "watch";
    std::vector<MenuChoice> choices;
    for (const char* id : {left, right}) {
        const auto* option = FindSeatOption(id);
        choices.push_back({id, loc::T(option->label), loc::T(option->detail)});
    }
    choices.push_back({"options", loc::T("room.legend_options"), loc::T("room.table_options")});
    return choices;
}
// What B does on the board for a member with a place at a table: leave it
// when the table allows, otherwise take Ready back, otherwise say why not.
// table < 0: no place, so B is the ordinary Back.
struct PlaceExit {
    int table = -1;
    room::ActionKind kind = room::ActionKind::Unqueue;
    bool allowed = false;
    const char* label = "";
    std::string blocker;
};
PlaceExit ExitFromPlace(const ShellView& v) {
    using room::TablePhase;
    PlaceExit place;
    const auto* local = Member(v.room, v.room.localMember);
    if (!local) return place;
    if (local->seat >= 0 && local->seat < 2 && local->table >= 0 && local->table < static_cast<int>(v.room.tables.size())) {
        const auto& t = v.room.tables[local->table];
        place.table = local->table; place.label = loc::T("room.leave_seat");
        // The same test SendRoom applies to a seated Unqueue.
        const bool active = t.phase == TablePhase::Ready || t.phase == TablePhase::Playing || t.phase == TablePhase::Paused ||
            v.session.match == netplay::MatchState::Preparing || v.session.match == netplay::MatchState::Playing;
        if (!active) { place.allowed = true; return place; }
        if (room::ReadyCancellable(t, local->seat)) {
            place.kind = room::ActionKind::Unready; place.allowed = true; place.label = loc::T("room.unready"); return place;
        }
        place.blocker = loc::T(t.phase == TablePhase::Paused ? "room.leave_seat.abandon_first" :
            t.phase == TablePhase::Playing && v.session.match == netplay::MatchState::PostMatch ? "room.awaiting_result" :
            "room.leave_seat.finish_first");
        return place;
    }
    for (const auto& t : v.room.tables)
        if (std::find(t.queue.begin(), t.queue.end(), local->id) != t.queue.end()) {
            place.table = t.id; place.allowed = true; place.label = loc::T("room.leave_queue"); return place;
        }
    return place;
}
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
// The Ready control, described once for the board (card strip and legend),
// the table options row and dispatch, so none of them offers a press another
// refuses. kind None: nothing to send, and refusal says why. detail is the
// row's explanation, which a brief room update must not flicker.
struct ReadyControl {
    enum Kind { None, Ready, Rematch, Unready } kind = None;
    const char* label = "";
    std::string detail, refusal;
};
ReadyControl DescribeReady(const ShellView& v, const room::Table& t, int seat) {
    using room::TablePhase;
    ReadyControl control;
    if (seat >= 0 && seat < 2 && room::ReadyCancellable(t, seat)) {
        control.label = loc::T("room.unready");
        control.detail = loc::T(t.spectatorHold ? "room.waiting_spectators" : "room.ready.cancel_detail");
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
    }
    return control;
}
// The table the options open without a card to say otherwise: your own
// place's, else the last table card you were on.
int OptionsTable(const ShellView& v, int selected) {
    const auto* local = Member(v.room, v.room.localMember);
    return local && local->table >= 0 && local->table < static_cast<int>(v.room.tables.size()) ? local->table : selected;
}
// A member with no seat and no queue place anywhere gets the chooser from a
// table card. Watching elsewhere is no obstacle: sitting down ends it.
bool ChoosesSeat(const room::Snapshot& s) {
    const auto* local = Member(s, s.localMember);
    return local && local->seat < 0 && std::none_of(s.tables.begin(), s.tables.end(), [&](const room::Table& t) {
        return std::find(t.queue.begin(), t.queue.end(), s.localMember) != t.queue.end();
    });
}
}
bool ApplicationShell::SendRoom(room::Action action, const ShellView& view, const Submit& submit) {
    // Button disabled state is a rendering affordance; keyboard/controller
    // activation can still reach an item ID. Recheck the room authority
    // boundary at dispatch so a lost or closed control stream never queues a
    // room mutation.
    if (!RoomActionsAvailable(view) || !view.room.roomEpoch || !view.room.localMember) {
        error_ = RoomWaitReason(view); return false;
    }
    if (action.table >= view.room.tables.size()) return false;
    const auto* local = Member(view.room, view.room.localMember);
    const auto& table = view.room.tables[action.table];
    if (TerminalFenced(view.room, action.kind, action.table)) {
        error_ = TerminalPendingReason();
        return false;
    }
    const bool localSeated = local && local->table == static_cast<std::int8_t>(action.table) &&
        local->seat >= 0 && local->seat < 2;
    const bool tableActive = table.phase == room::TablePhase::Ready || table.phase == room::TablePhase::Playing ||
        table.phase == room::TablePhase::Paused;
    if ((action.kind == room::ActionKind::Queue || action.kind == room::ActionKind::Watch) && localSeated) return false;
    if (action.kind == room::ActionKind::AbortMatch && (!localSeated || table.phase != room::TablePhase::Paused)) return false;
    if (action.kind == room::ActionKind::Unqueue && localSeated &&
        (tableActive || view.session.match == netplay::MatchState::Preparing || view.session.match == netplay::MatchState::Playing)) return false;
    action.protocolVersion = room::ProtocolVersion;
    action.roomEpoch = view.room.roomEpoch;
    action.revision = view.room.revision;
    if (action.table < view.room.tables.size()) {
        action.tableRevision = view.room.tables[action.table].revision;
		const bool generationScoped = action.kind == room::ActionKind::RecordResult ||
			action.kind == room::ActionKind::MatchFinished || action.kind == room::ActionKind::CancelResult ||
			action.kind == room::ActionKind::AbortMatch ||
			(action.kind == room::ActionKind::Unwatch &&
				(table.phase == room::TablePhase::Playing || table.phase == room::TablePhase::Paused));
		if (!action.matchGeneration && generationScoped)
			action.matchGeneration = view.room.tables[action.table].matchGeneration;
    }
    action.actionId = nextActionId_++;
    ShellAction request;
    request.command.kind = netplay::CommandKind::RoomAction;
    request.command.generation = view.session.generation;
    request.roomAction = std::move(action);
    if (!submit(std::move(request))) { error_ = loc::T("error.queue_failed"); return false; }
    error_.clear(); return true;
}


std::vector<MenuEntry> ApplicationShell::RoomEntries(const ShellView& v) {
 using namespace room;
 auto& nav=menu_.navigation; const auto& s=v.room;
 if(roomEpoch_!=s.roomEpoch){roomEpoch_=s.roomEpoch;muted_.clear();chat_[0]=0;selectedTable_=0;rulesDirty_=false;
  std::snprintf(roomName_,sizeof(roomName_),"%s",s.name.c_str());roomCapacity_=s.capacity;
  if(const auto* local=Member(s,s.localMember))if(local->table>=0&&local->table<static_cast<int>(s.tables.size()))selectedTable_=local->table;
 }
 std::vector<MenuEntry> rows;
  const bool mutableRoom=RoomActionsAvailable(v);
  const bool host=mutableRoom&&s.host==s.localMember;
 const auto* local=Member(s,s.localMember);
 const auto& t=s.tables[selectedTable_];
 // A new game at the same table closes a dialog about the last one; moving to
 // another table (a click opens its chooser a frame before this catches up)
 // does not.
 if(generationTable_!=selectedTable_){generationTable_=selectedTable_;tableGeneration_=t.matchGeneration;}
 else if(tableGeneration_!=t.matchGeneration){tableGeneration_=t.matchGeneration;nav.Cancel();}
 const bool active=t.phase==TablePhase::Ready||t.phase==TablePhase::Playing||t.phase==TablePhase::Paused;
 const bool at=local&&local->table==selectedTable_,seated=at&&local->seat>=0&&local->seat<2;
 const bool elsewhere=local&&local->table>=0&&!at;
 const bool playing=v.session.match==netplay::MatchState::Preparing||v.session.match==netplay::MatchState::Playing;
 const bool queued=std::find(t.queue.begin(),t.queue.end(),s.localMember)!=t.queue.end();
 const bool watching=std::find(t.spectators.begin(),t.spectators.end(),s.localMember)!=t.spectators.end()||
  std::find(t.watchingNext.begin(),t.watchingNext.end(),s.localMember)!=t.watchingNext.end();
 const auto screen=nav.Screen();
  if(screen=="room"){
  if(s.roomEpoch){
   for(const auto& table:s.tables){
    const auto occupied=(table.p1?1:0)+(table.p2?1:0);
    std::string detail=loc::Tf("room.table_detail",Name(s,table.p1),Name(s,table.p2),PhaseName(table.phase),
     table.queue.size(),table.spectators.size()+table.watchingNext.size(),local?StatusName(local->status):loc::T("room.connecting"));
    rows.push_back(Row("table-"+std::to_string(table.id),loc::Tf("room.table_occupancy",table.id+1,occupied),detail));
    if(ChoosesSeat(s))rows.back().choices=SeatChoices(table,s.localMember);
    else if(local&&local->table==static_cast<int>(table.id)&&local->seat>=0&&local->seat<2)rows.back().hint=DescribeReady(v,table,local->seat).label;
    else if(std::find(table.queue.begin(),table.queue.end(),s.localMember)!=table.queue.end())rows.back().hint=loc::T("room.table_options");
   }
   for(const auto& m:s.members){rows.push_back(Row("member-"+std::to_string(m.id),loc::Tf(m.id==s.localMember?"room.member_you":"room.member",m.name),loc::Tf(m.host?"room.member_status_host":"room.member_status",StatusName(m.status))));
    rows.back().userText=true;}
   rows.push_back(Row("room-members",loc::T("room.members"),loc::Tf("room.member_count",s.members.size(),s.capacity)));
   rows.push_back(Row("room-chat",loc::T("room.chat"),loc::T("room.chat.detail")));
   // Changing fighter needs a seat, so it lives in the table options (and X).
   rows.push_back(Row("options",loc::T("room.table_options"),loc::Tf("room.table_options.detail",OptionsTable(v,selectedTable_)+1)));
   rows.push_back(Row("room-admin",loc::T("room.settings"),loc::T(host?"room.settings.detail":"room.settings.host_only"),host));
  }else{
   rows.push_back(Row("room-status",loc::T("room.connection_status"),loc::T(v.session.room==netplay::RoomState::Opening?"room.opening":"room.waiting_state"),false));
   if(v.canReady)rows.push_back(Row("ready",loc::T("room.ready"),loc::T("room.ready.saved_fighter"),mutableRoom));
  }
  rows.push_back(Row("copy",loc::T("room.copy_invitation"),loc::T("room.copy_invitation.detail"),!v.invitation.empty()));
  rows.push_back(ConfirmRow("leave",loc::T(v.session.room==netplay::RoomState::Closing?"room.leaving":"room.leave"),
   LeaveRoomDetail(v),v.session.room!=netplay::RoomState::Closing));
  if(v.session.recovery==netplay::Recovery::ReplacementOffered)
   rows.push_back(ConfirmRow("replace-room",loc::T("room.replace"),loc::T(v.canReplaceRoom?"room.replace.detail":"room.replace.waiting"),v.canReplaceRoom));
  for(std::size_t i=0;i<rows.size();++i){auto& e=rows[i];
   if(e.id.compare(0,6,"table-")==0)e.right=s.members.empty()?"room-members":"member-"+std::to_string(s.members.front().id);
   else if(e.id.compare(0,7,"member-")==0)e.left="table-"+std::to_string(selectedTable_);
   else {e.left=i?rows[i-1].id:e.id;e.right=i+1<rows.size()?rows[i+1].id:e.id;}
  }
  }else if(screen=="room-table"){
   const std::string reason=!mutableRoom?RoomWaitReason(v):
    loc::T(elsewhere?"room.leave_current_table":"room.choose_action");
  if(seated){
   const bool ready=room::ReadyCancellable(t,local->seat);
   const bool terminalBlocked=TerminalFenced(s,room::ActionKind::Ready,selectedTable_);
   const bool awaitingResult=t.phase==TablePhase::Playing&&(t.resultPending||v.session.match==netplay::MatchState::PostMatch);
   const std::string blocked=!mutableRoom&&!(active&&RoomCheckpointPending(v))?reason:
    terminalBlocked?TerminalPendingReason():
    t.phase==TablePhase::Paused?loc::T("room.result_unresolved.detail"):
    awaitingResult?loc::T("room.awaiting_result"):
    active?loc::T("room.match_active"):
    !t.p1||!t.p2?loc::T("room.waiting_other_seat"):
    !v.controllerReady?loc::T("room.controller_required"):
    !v.selectionError.empty()?v.selectionError:!v.readyLockReason.empty()?v.readyLockReason:loc::T("room.waiting_update");
    const auto readyControl=DescribeReady(v,t,local->seat);
    rows.push_back(Row("ready",readyControl.label,readyControl.detail,readyControl.kind!=ReadyControl::None));
    const bool delayEditable=mutableRoom&&!active&&!v.delayLocked;
    const int selectedDelay=(std::max)(0,(std::min)(MaximumInputDelay,v.selectedDelay));
    const bool recommended=v.recommendedDelay>=0&&v.recommendedDelay<=MaximumInputDelay;
    const auto check=DescribeConnectionCheck(v);
    rows.push_back(ReadOnlyValue("recommended-delay",loc::T("room.recommended_delay"),check.value,check.detail));
    rows.push_back(Value("selected-delay",loc::T("room.selected_delay"),std::to_string(selectedDelay),
     delayEditable?loc::T("room.selected_delay.detail"):
      (v.delayLocked?loc::T("room.selected_delay.locked"):reason),delayEditable));
    if(t.p1&&t.p2) {
     const bool opponentReady=v.opponentDelay>=0&&v.opponentDelay<=MaximumInputDelay;
     rows.push_back(ReadOnlyValue("match-delay",loc::T("room.match_delay"),
      opponentReady?loc::Tf("connection.frames",room::MatchDelay(selectedDelay,v.opponentDelay)):
       loc::Tf("room.match_delay.at_least",selectedDelay),
      opponentReady?loc::Tf("room.match_delay.detail",selectedDelay,v.opponentDelay):
       std::string(loc::T("room.match_delay.pending"))));
    }
    rows.push_back(Row("check-connection",check.action,check.checking?check.detail:
     !mutableRoom?reason:!t.p1||!t.p2?loc::T("room.check_connection.two_players"):
     ready?loc::T("room.check_connection.unready"):
     !delayEditable?loc::T("room.check_connection.finish_match"):check.detail,
     v.canProbe&&delayEditable&&!check.checking));
    rows.push_back(Row("apply-recommendation",loc::T("room.apply_recommendation"),recommended?
     loc::Tf("room.apply_recommendation.detail",v.recommendedDelay):loc::T("room.apply_recommendation.check_first"),
     v.canApplyDelay&&recommended&&delayEditable&&!check.checking));
   const std::string editReason=active?blocked:ready?loc::T("room.change_fighter.unready"):
     !mutableRoom?reason:!v.selectionLockReason.empty()?v.selectionLockReason:loc::T("room.change_fighter.waiting");
   rows.push_back(Row("selection",loc::T("room.change_fighter"),mutableRoom&&v.canEditSelection&&!s.localTerminalPending?
    std::string(loc::T("room.change_fighter.detail"))+"\n"+v.selectionSummary:
    (s.localTerminalPending?TerminalPendingReason():editReason),mutableRoom&&v.canEditSelection&&!s.localTerminalPending));
    rows.push_back(Row("unqueue",loc::T("room.leave_seat"),loc::T(t.phase==TablePhase::Paused?"room.leave_seat.abandon_first":
     active?"room.leave_seat.finish_first":
     t.queue.empty()?"room.leave_seat.release":"room.leave_seat.next_player"),mutableRoom&&!active&&!playing));
    // The authority accepts AbortMatch from either fighter. Offer it only once
    // the result is Paused, so a fighter can never cut a live game short.
    if(t.phase==TablePhase::Paused)rows.push_back(ConfirmRow("abandon-result",loc::T("room.abandon_result"),
     loc::T("room.abandon_result.detail"),mutableRoom));
   }else{
    // Enablement asks the same fence SendRoom dispatches through, so the row
    // and the dispatch guard cannot drift apart.
    const auto tableRow=[&](room::ActionKind kind,const char* id,const char* label){
     const bool fenced=TerminalFenced(s,kind,selectedTable_);
     rows.push_back(Row(id,loc::T(label),fenced?TerminalPendingReason():reason,mutableRoom&&!elsewhere&&!fenced));
    };
    tableRow(queued?room::ActionKind::Unqueue:room::ActionKind::Queue,queued?"unqueue":"queue",
     queued?"room.leave_queue":"room.join_queue");
    tableRow(watching?room::ActionKind::Unwatch:room::ActionKind::Watch,watching?"unwatch":"watch",
     watching?"room.stop_watching":"room.watch_next");
    // A watcher's lock-in is never fenced: the one still leaving the last
    // game is exactly who it is for.
    if(room::WatchesByChoice(t,s.localMember)){
     const bool lockedIn=local&&local->spectatorLocked;
     rows.push_back(Row("lock-spectating",loc::T(lockedIn?"room.unlock_spectating":"room.lock_spectating"),
      mutableRoom?loc::T(lockedIn?"room.unlock_spectating.detail":"room.lock_spectating.detail"):reason,mutableRoom));
    }
  }
  rows.push_back(Row("room-rules",loc::T("room.table_rules"),loc::T(host?"room.table_rules.edit":"room.table_rules.view")));
  if(t.phase==TablePhase::Paused)rows.push_back(ConfirmRow("cancel-result",loc::T("room.cancel_unresolved"),loc::T("room.cancel_unresolved.detail"),host));
  // A table can be left in Playing when neither fighter's finish report
  // reached the room (both clients lost control at battle close). The
  // authority accepts a host cancel in that phase; offer it to a host who is
  // not one of the fighters, so a live game can never be cut short by its
  // own participant from this row.
  else if(t.phase==TablePhase::Playing&&host&&!seated&&!t.resultPending)
   rows.push_back(ConfirmRow("cancel-result",loc::T("room.cancel_stuck"),loc::T("room.cancel_stuck.detail"),true));
  if(v.session.recovery==netplay::Recovery::ReplacementOffered)
   rows.push_back(ConfirmRow("replace-room",loc::T("room.replace"),loc::T(v.canReplaceRoom?"room.replace.detail":"room.replace.waiting"),v.canReplaceRoom));
  rows.push_back(ConfirmRow("leave",loc::T(v.session.room==netplay::RoomState::Closing?"room.leaving":"room.leave"),
   LeaveRoomDetail(v),v.session.room!=netplay::RoomState::Closing));
 }else if(screen=="room-rules"){
  if(!rulesDirty_&&rulesRevision_!=t.revision){tableRules_=t.rules;rulesRevision_=t.revision;}
  RuleRows(rows,tableRules_,host&&!active,loc::T(host?(active?"room.rules.finish_game":"room.rules.apply_note"):"room.rules.host_only"));
  rows.push_back(Row("apply-rules",loc::T("room.apply_rules"),loc::T("room.apply_rules.detail"),host&&!active&&rulesDirty_));
 }else if(screen=="room-members"){
  for(const auto& m:s.members){
   rows.push_back(Row("member-"+std::to_string(m.id),loc::Tf(m.id==s.localMember?"room.member_you":"room.member",m.name),
    loc::Tf(m.host?(muted_.count(m.id)?"room.member_status_host_muted":"room.member_status_host"):(muted_.count(m.id)?"room.member_status_muted":"room.member_status"),StatusName(m.status))));
   rows.back().value=NetworkLinkName(m.link);rows.back().userText=true;
  }
  }else if(screen=="room-member"){
   const auto* m=Member(s,selectedMember_);const bool other=m&&m->id!=s.localMember;
   rows.push_back(Row("mute",loc::T(muted_.count(selectedMember_)?"room.unmute_member":"room.mute_member"),loc::T(m?"room.mute_member.detail":"room.member_left.detail"),other));
   rows.push_back(ConfirmRow("kick",loc::T("room.kick_member"),m?loc::Tf("room.kick_member.detail",m->name):loc::T("room.member_left.detail"),host&&other));
   rows.push_back(ConfirmRow("transfer-host",loc::T("room.transfer_host"),m?loc::Tf("room.transfer_host.detail",m->name):loc::T("room.member_left.detail"),host&&other));
 }else if(screen=="room-chat"){
   rows.push_back(TextRow("compose",loc::T("room.compose_message"),chat_,MaximumChatBytes,mutableRoom));
   rows.push_back(Row("send-chat",loc::T("room.send_message"),loc::T("room.send_message.detail"),mutableRoom&&chat_[0]));
  for(auto it=s.chat.rbegin();it!=s.chat.rend();++it)if(!muted_.count(it->sender)){
   rows.push_back(Row("message-"+std::to_string(it->sequence),Name(s,it->sender),it->text));rows.back().userText=true;}
 }else if(screen=="room-admin"){
  rows.push_back(TextRow("rename",loc::T("room.name"),roomName_,64,host));
  rows.push_back(Value("room-capacity",loc::T("room.capacity"),std::to_string(roomCapacity_),loc::T("room.capacity.detail"),host));
  rows.push_back(Value("lock",loc::T("room.admission"),loc::T(s.locked?"room.locked":"room.open"),loc::T("room.admission.detail"),host));
  rows.push_back(Row("apply-name",loc::T("room.apply_name"),loc::T("room.apply_name.detail"),host&&roomName_[0]));
  rows.push_back(Row("apply-capacity",loc::T("room.apply_capacity"),loc::T("room.apply_capacity.detail"),host&&roomCapacity_>=static_cast<int>(s.members.size())));
 }
 if(!active) {
  for(auto& row:rows)if(row.id=="selection"||row.id=="check-connection"||
   row.id=="selected-delay"||row.id=="queue"||row.id=="watch") {
    const auto key=screen+"/"+row.id;
    if(roomUpdateVisible_)row.detail=RoomWaitReason(v);
    else if(RoomCheckpointPending(v)) {
     // Only reuse an explanation for the same action. Labels, values and
     // eligibility always come from the live snapshot, never this cache.
     const auto previous=roomDetails_.find(key);
     if(row.detail==RoomWaitReason(v)&&previous!=roomDetails_.end())
      row.detail=previous->second;
    }else if(mutableRoom)roomDetails_[key]=row.detail;
   }
 }
 return rows;
}
void ApplicationShell::DrawRoomBoard(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height,
                                     const MenuVisualFeedback& feedback) {
 const float s=Scale(),gap=12*s;const bool wide=ImGui::GetContentRegionAvail().x>=820*s;
 const auto focus=nav.Focus();const bool changed=focus!=roomBoardFocus_;
 // The board renders in place of the menu list, so it uses the same smoothed
 // verdict and the same wording. Presentation only: nav.Choose still gates on
 // the live entry and SendRoom still re-checks room authority at dispatch.
 const auto shown=[&](const MenuEntry& e){return feedback.Enabled(e);};
 const auto hint=[&](const MenuEntry& e){return feedback.Enabled(e)?"":loc::T(feedback.Pending(e)?"room.updating":"common.unavailable");};
 const auto origin=ImGui::GetCursorScreenPos();
 const float fullHeight=height;
 if(!wide)height=(std::max)(80*s,height-64*s);
 if(focus.compare(0,6,"table-")==0)selectedTable_=std::stoi(focus.substr(6));
 // B on your own table's card leaves your place there (or takes Ready back
 // where leaving has to wait); anywhere else on the board it is the ordinary
 // Back, so Home stays reachable without giving up the place.
 const auto place=ExitFromPlace(v);
 const bool onOwnPlace=place.table>=0&&focus=="table-"+std::to_string(place.table);
 if(action.kind==MenuAction::Back&&onOwnPlace)action={MenuAction::Activate,"leave-place"};
 std::string elided;
 const auto text=[&](ImVec2 p,float width,const std::string& value,float size,ImU32 color,bool centred=false){
  if(width<=0)return;
  auto* font=ImGui::GetFont();std::string label=value;
  float measured=font->CalcTextSizeA(size,FLT_MAX,0,label.c_str()).x;
  if(measured>width){
   const char* end=nullptr;const float dots=font->CalcTextSizeA(size,FLT_MAX,0,"...").x;
   font->CalcTextSizeA(size,(std::max)(1.f,width-dots),0,label.c_str(),nullptr,&end);
   label=std::string(label.c_str(),end)+"...";
   elided+=(elided.empty()?"":"\n")+value;
   measured=font->CalcTextSizeA(size,FLT_MAX,0,label.c_str()).x;
  }
  if(centred)p.x+=(std::max)(0.f,(width-measured)*.5f);
  ImGui::GetWindowDrawList()->AddText(font,size,p,color,label.c_str());
 };
 // Flushed once per row, while the row's button is still the hovered item.
 const auto tip=[&]{
  if(!elided.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",elided.c_str());
  elided.clear();
 };
 const auto press=[&](const MenuEntry& e,ImVec2 size){
  ImGui::PushID(e.id.c_str());
  ImGui::PushStyleColor(ImGuiCol_Button,e.id==focus?ImVec4(.40f,.23f,.12f,.9f):ImVec4(.10f,.09f,.08f,.94f));
  // An open choice leaves only its own options live.
  ImGui::BeginDisabled(nav.Choosing());
  const bool clicked=ImGui::Button("##room-entry",size);
  ImGui::EndDisabled();
  ImGui::PopStyleColor();ImGui::PopID();
  if(clicked&&!nav.Confirming()&&!nav.Editing()){nav.Focus(e.id,rows);action=nav.Choose(rows);}
  if(e.id==focus){
   ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),palette::Ember,0,0,2*s);
   if(changed)ImGui::SetScrollHereY(.5f);
  }
 };
 const auto fighter=[&](const room::Member* m){return m?(m->id==v.room.localMember?v.selectedFighter:m->fighter):-1;};
 // The highlighted option of the entry's open choice, in full; empty otherwise.
 const auto choiceDetail=[&](const MenuEntry& e)->std::string{
  return nav.Choosing()&&nav.DialogId()==e.id&&nav.ChoiceIndex()<e.choices.size()?e.choices[nav.ChoiceIndex()].detail:std::string();
 };
 // Options over a card's footer. They are real buttons, submitted before the
 // card so they own the hover and popups still block them; the card is then
 // laid out from the same origin.
 struct Strip{std::vector<ImVec2> min,max;int clicked=-1,hovered=-1;};
 const auto stripButtons=[&](const MenuEntry& e,ImVec2 p,float width,float h,std::size_t count){
  Strip strip;strip.min.resize(count);strip.max.resize(count);
  const float stripHeight=26*s,y=p.y+h-stripHeight-3*s,optionWidth=(width-24*s-(count-1)*8*s)/count;
  ImGui::PushID(e.id.c_str());
  for(int i=0;i<static_cast<int>(count);++i){
   strip.min[i]=ImVec2(p.x+12*s+i*(optionWidth+8*s),y);strip.max[i]=ImVec2(strip.min[i].x+optionWidth,y+stripHeight);
   ImGui::SetCursorScreenPos(strip.min[i]);ImGui::PushID(i);
   if(ImGui::InvisibleButton("##strip",ImVec2(optionWidth,stripHeight)))strip.clicked=i;
   if(ImGui::IsItemHovered())strip.hovered=i;
   ImGui::PopID();ReportMenuCard((e.id+"/"+std::to_string(i)).c_str(),strip.min[i],strip.max[i]);
  }
  ImGui::PopID();ImGui::SetCursorScreenPos(p);
  return strip;
 };
 const auto drawStrip=[&](const Strip& strip,const std::vector<std::string>& labels,int chosen,const std::vector<bool>& live){
  auto* d=ImGui::GetWindowDrawList();
  for(int i=0;i<static_cast<int>(labels.size());++i){
   const bool lit=chosen>=0?i==chosen:i==strip.hovered;
   d->AddRectFilled(strip.min[i],strip.max[i],lit?IM_COL32(120,64,30,245):IM_COL32(20,19,18,240),4*s);
   if(lit)d->AddRect(strip.min[i],strip.max[i],palette::Ember,4*s,0,2*s);
   const float stripHeight=strip.max[i].y-strip.min[i].y,optionWidth=strip.max[i].x-strip.min[i].x;
   // Our own action labels must fit; the probe holds every locale to that.
   ReportMenuText(("board-strip-"+labels[i]).c_str(),14*s,stripHeight,
    ImGui::GetFont()->CalcTextSizeA(14*s,FLT_MAX,0,labels[i].c_str()).x,optionWidth-12*s);
   text(ImVec2(strip.min[i].x+6*s,strip.min[i].y+(stripHeight-14*s)*.5f),optionWidth-12*s,labels[i],14*s,live[i]?palette::Ivory:palette::Muted,true);
  }
 };
 const auto tableCard=[&](const MenuEntry& e,float h){
  const auto& t=v.room.tables[std::stoi(e.id.substr(6))];const auto p=ImGui::GetCursorScreenPos();
  const float width=ImGui::GetContentRegionAvail().x;
  // The seat chooser, or on your own focused place what A and B do there.
  const bool choosing=nav.Choosing()&&nav.DialogId()==e.id&&!e.choices.empty();
  const bool ownPlace=!nav.Confirming()&&onOwnPlace&&e.id==focus&&!e.hint.empty();
  std::vector<std::string> labels;
  if(choosing)for(const auto& c:e.choices)labels.push_back(c.label);
  else if(ownPlace)labels={e.hint,place.label};
  const Strip strip=labels.empty()?Strip{}:stripButtons(e,p,width,h,labels.size());
  press(e,ImVec2(width,h));ReportMenuCard(e.id.c_str(),p,ImVec2(p.x+width,p.y+h));
  text(ImVec2(p.x+12*s,p.y+7*s),width*.57f,loc::Tf("room.battle_slot",t.id+1),16*s,palette::Ivory);
  text(ImVec2(p.x+width*.60f,p.y+8*s),width*.40f-12*s,PhaseName(t.phase),14*s,palette::Ember);
  // A full pair shows its running win count where "VS" would sit; the gap
  // between the two sides grows to fit however long the rematch run gets.
  const std::string middle=t.p1&&t.p2?SetScoreText(t.score):"VS";
  const float gap=(std::max)(34*s,ImGui::GetFont()->CalcTextSizeA(18*s,FLT_MAX,0,middle.c_str()).x+12*s);
  // Portraits sit on the card's outer edges, mirrored, so neither crowds the
  // score. The row fills and centres in the band between header and footer.
  // The footer band holds the rules line or, over it, an option strip, so
  // portraits and names never sit under the strip.
  const float band=h-60*s,portrait=(std::min)(96*s,band),half=(width-24*s-gap)*.5f;
  const float top=p.y+28*s+(band-portrait)*.5f+portrait*.5f-20*s;
  const room::MemberId ids[]={t.p1,t.p2};
  for(int side=0;side<2;++side){
   const float x=p.x+12*s+side*(half+gap);const auto* m=Member(v.room,ids[side]);const int id=fighter(m);
   const float px=side?x+half-portrait:x,py=p.y+28*s+(band-portrait)*.5f;
   if(m)DrawCharacterPortrait(id,ImVec2(px,py),ImVec2(px+portrait,py+portrait));
   // The link mark rides the portrait's inner lower corner on a dark disc.
   if(m){
    const float mark=14*s;const ImVec2 centre(side?px+10*s:px+portrait-10*s,py+portrait-10*s);
    ImGui::GetWindowDrawList()->AddCircleFilled(centre,11*s,IM_COL32(20,19,18,225));
    DrawNetworkLinkGlyph(ImGui::GetWindowDrawList(),ImVec2(centre.x-mark*.5f,centre.y-mark*.5f),mark,m->link);
   }
   // A readied fighter wears a badge on the portrait until the game starts.
   if(m&&t.ready[side]&&(t.phase==room::TablePhase::Waiting||t.phase==room::TablePhase::Ready)){
    const char* badge=loc::T("room.status.ready");const float size=13*s;
    const float badgeWidth=ImGui::GetFont()->CalcTextSizeA(size,FLT_MAX,0,badge).x+12*s;
    const ImVec2 min(px+(portrait-badgeWidth)*.5f,py+4*s),max(min.x+badgeWidth,min.y+size+4*s);
    ImGui::GetWindowDrawList()->AddRectFilled(min,max,palette::Ready,(size+4*s)*.5f);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),size,ImVec2(min.x+6*s,min.y+2*s),IM_COL32(20,19,18,255),badge);
   }
   const float tx=x+(m&&!side?portrait+8*s:0),tw=half-(m?portrait+8*s:0);
   // Names of different lengths read ragged when flush left; centre each
   // in its own slot so the pair stays symmetric about VS.
   text(ImVec2(tx,top),tw,m?m->name:loc::T("room.looking_for_fight"),18*s,m?palette::Ivory:palette::Muted,true);
   const auto* f=selection::FindFighter(id);
   // An empty seat already reads "Looking for a fight" above; a second
   // "Open seat" underneath said nothing new.
   std::string caption=m?(f?f->name:loc::T("room.fighter_not_shared")):std::string();
   if(m&&m->id==v.room.localMember)caption+=loc::T("room.suffix_you");
   if(!caption.empty())text(ImVec2(tx,top+23*s),tw,caption,14*s,palette::Muted,true);
  }
  text(ImVec2(p.x+12*s+half,top+16*s),gap,middle,18*s,palette::Ember,true);
  // An option strip covers the footer, so the footer gives way to it.
  if(!choosing&&!ownPlace)text(ImVec2(p.x+12*s,p.y+h-22*s),width-24*s,loc::Tf("room.table_footer",t.rules.roundCount,t.rules.roundTime,t.queue.size(),t.spectators.size()+t.watchingNext.size()),14*s,palette::Muted);
  if(choosing){
   // The chosen side follows the mouse, and the seat a sit option would take
   // blinks.
   // Only a moving mouse takes the choice; a resting cursor must not undo
   // what the pad or keyboard just picked.
   const auto& io=ImGui::GetIO();
   if(strip.hovered>=0&&(io.MouseDelta.x!=0||io.MouseDelta.y!=0))nav.ChoiceIndex(static_cast<std::size_t>(strip.hovered));
   const int chosen=static_cast<int>(nav.ChoiceIndex());
   const auto* option=FindSeatOption(e.choices[chosen].id);
   if(option&&option->seat>=0){
    const float pulse=.55f+.45f*std::sin(static_cast<float>(ImGui::GetTime())*6.f);
    const float x=p.x+12*s+option->seat*(half+gap);
    ImGui::GetWindowDrawList()->AddRect(ImVec2(x-4*s,p.y+26*s),ImVec2(x+half+4*s,p.y+h-32*s),ImGui::GetColorU32(ImVec4(1.f,.53f,.22f,pulse)),4*s,0,3*s);
   }
   drawStrip(strip,labels,chosen,std::vector<bool>(labels.size(),true));
   if(strip.clicked>=0)action=nav.Pick(static_cast<std::size_t>(strip.clicked),rows);
  }else if(ownPlace){
   const auto* local=Member(v.room,v.room.localMember);
   const bool readyLive=!local||local->seat<0||DescribeReady(v,t,local->seat).kind!=ReadyControl::None;
   drawStrip(strip,labels,-1,{shown(e)&&readyLive,place.allowed});
   if(strip.clicked==0){nav.Focus(e.id,rows);action=nav.Choose(rows);}
   else if(strip.clicked==1)action={MenuAction::Activate,"leave-place"};
  }
  tip();
 };
 const auto memberCard=[&](const MenuEntry& e){
  const auto* m=Member(v.room,std::stoull(e.id.substr(7)));if(!m)return;
  const auto p=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
  const int main=m->id==v.room.localMember?v.preferences.mainFighter:m->mainFighter;
  press(e,ImVec2(width,58*s));DrawCharacterPortrait(main,ImVec2(p.x+6*s,p.y+5*s),ImVec2(p.x+54*s,p.y+53*s));
  text(ImVec2(p.x+64*s,p.y+7*s),width-98*s,e.label,16*s,palette::Ivory);
  DrawNetworkLinkGlyph(ImGui::GetWindowDrawList(),ImVec2(p.x+width-28*s,p.y+8*s),16*s,m->link);
  // A controller player reads the mark (or the Members list); a mouse can
  // also have it in words.
  elided+=(elided.empty()?"":"\n")+std::string(NetworkLinkName(m->link));
  const auto status=loc::Tf(m->table>=0?(m->host?"room.board_status_host_slot":"room.board_status_slot"):(m->host?"room.board_status_host":"room.board_status"),StatusName(m->status),m->table+1)+
   (m->spectatorLocked?loc::T("room.suffix_locked_in"):"");
  text(ImVec2(p.x+64*s,p.y+31*s),width-76*s,status,14*s,palette::Muted);
  tip();
 };
 const auto button=[&](const MenuEntry& e,float width){
  const auto p=ImGui::GetCursorScreenPos();press(e,ImVec2(width,40*s));
  // Our own action labels must fit; only player-supplied text may elide.
  ReportMenuText(("board-"+e.id).c_str(),16*s,40*s,
   ImGui::GetFont()->CalcTextSizeA(16*s,FLT_MAX,0,e.label.c_str()).x,width-24*s);
  text(ImVec2(p.x+12*s,p.y+11*s),width-24*s,e.label,16*s,shown(e)?palette::Ivory:palette::Muted);
  tip();
 };
 std::vector<const MenuEntry*> toolbar;
 for(const auto& e:rows)if(e.id.compare(0,6,"table-")&&e.id.compare(0,7,"member-"))toolbar.push_back(&e);
 ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(6*s,6*s));
 ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.08f,.075f,.07f,.84f));
 if(wide){
  // The battle slots take the whole height on the left. Members, chat and the
  // room's actions share the right, and the focused row's explanation runs
  // under both.
  const auto origin=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
  const float explanationHeight=ImGui::GetFontSize()+4*s;
  const float boardHeight=(std::max)(180*s,height-explanationHeight-gap),leftWidth=(width-gap)*.50f,rightWidth=width-leftWidth-gap;
  const int toolColumns=rightWidth>=3*170*s?3:2;
  const float toolsHeight=std::ceil(toolbar.size()/float(toolColumns))*48*s+8*s;
  const float communityHeight=(std::max)(120*s,boardHeight-toolsHeight-gap);
  ImGui::BeginChild("Battle slots",ImVec2(leftWidth,boardHeight),0,ImGuiWindowFlags_NoNavInputs);
  const float rowHeight=(std::max)(100*s,(ImGui::GetContentRegionAvail().y-3*8*s)/4);
  for(const auto& e:rows)if(e.id.compare(0,6,"table-")==0)tableCard(e,rowHeight);
  ImGui::EndChild();ImGui::SameLine(0,gap);
  ImGui::BeginChild("Room community",ImVec2(0,communityHeight),0,ImGuiWindowFlags_NoNavInputs);
  ImGui::TextUnformatted(loc::Tf("room.members_heading",v.room.members.size(),v.room.capacity).c_str());
  // Whole cards only. A free fraction of the board height always clipped the
  // bottom card through its portrait.
  const float cardPitch=58*s+ImGui::GetStyle().ItemSpacing.y,childPadding=12*s;
  const int memberRows=(std::max)(1,static_cast<int>(((communityHeight-48*s)*.56f-childPadding+ImGui::GetStyle().ItemSpacing.y)/cardPitch));
  ImGui::BeginChild("Member list",ImVec2(0,memberRows*cardPitch-ImGui::GetStyle().ItemSpacing.y+childPadding),0,ImGuiWindowFlags_NoNavInputs);
  for(const auto& e:rows)if(e.id.compare(0,7,"member-")==0)memberCard(e);
  ImGui::EndChild();ImGui::TextUnformatted(loc::T("room.chat_heading"));
  ImGui::BeginChild("Recent chat",ImVec2(0,0),0,ImGuiWindowFlags_NoNavInputs);
  if(v.room.chat.empty())ImGui::TextWrapped("%s",loc::T("room.no_messages"));
  for(const auto& message:v.room.chat)if(!muted_.count(message.sender)){
   ImGui::PushStyleColor(ImGuiCol_Text,ToneColor(Tone::Pending));
   ImGui::TextWrapped("%s",Name(v.room,message.sender));ImGui::PopStyleColor();ImGui::TextWrapped("%s",message.text.c_str());
  }
  // Follow new messages only while the reader is already at the bottom.
  if(ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-1||chatSequence_==0)ImGui::SetScrollHereY(1.f);
  chatSequence_=v.room.chat.empty()?0:v.room.chat.back().sequence;
  ImGui::EndChild();ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(origin.x+leftWidth+gap,origin.y+communityHeight+gap));
  ImGui::BeginChild("Room actions",ImVec2(rightWidth,toolsHeight),0,ImGuiWindowFlags_NoNavInputs);
  const float w=(ImGui::GetContentRegionAvail().x-(toolColumns-1)*8*s)/toolColumns;
  for(std::size_t i=0;i<toolbar.size();++i){if(i%toolColumns)ImGui::SameLine(0,8*s);button(*toolbar[i],w);}
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+boardHeight+gap));
  ImGui::BeginChild("Room explanation",ImVec2(width,explanationHeight),0,ImGuiWindowFlags_NoNavInputs|ImGuiWindowFlags_NoScrollbar);
  const auto e=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& row){return row.id==nav.Focus();});
  if(e!=rows.end()){
   // The wide board has one line for the focused row's explanation. Lines
   // after the first (the fighter summary under Ready, for example) and any
   // elided text are reachable on hover, like the narrow board's cards.
   const auto newline=e->detail.find('\n');
   std::string line=choiceDetail(*e).empty()?e->detail.substr(0,newline):choiceDetail(*e);
   const std::string reason=hint(*e);
   if(!reason.empty())line=line.empty()?reason:line+"  -  "+reason;
   const auto p=ImGui::GetCursorScreenPos();const float lineWidth=ImGui::GetContentRegionAvail().x;
   text(p,lineWidth,line,12*s,reason.empty()?palette::Muted:ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending)));
   if(newline!=std::string::npos)elided+=(elided.empty()?"":"\n")+e->detail;
   ImGui::Dummy(ImVec2(lineWidth,ImGui::GetFontSize()));
   tip();
  }
  ImGui::EndChild();
 }else{
  ImGui::BeginChild("Room stacked",ImVec2(0,height),0,ImGuiWindowFlags_NoNavInputs);
  for(const auto& e:rows){
   if(e.id.compare(0,6,"table-")==0)tableCard(e,136*s);
   else if(e.id.compare(0,7,"member-")==0)memberCard(e);
   else button(e,ImGui::GetContentRegionAvail().x);
  }
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+height+4*s));
  ImGui::BeginChild("Room action explanation",ImVec2(0,fullHeight-height-4*s));
  const auto selected=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==nav.Focus();});
  if(selected!=rows.end()){
   std::string explanation=selected->detail;
   if(!choiceDetail(*selected).empty())explanation=choiceDetail(*selected);
   else if(selected->id.compare(0,6,"table-")==0){const auto& table=v.room.tables[selectedTable_];const auto* local=Member(v.room,v.room.localMember);
    explanation=loc::Tf("room.table_explanation",PhaseName(table.phase),local?StatusName(local->status):loc::T("room.connecting"),table.queue.size(),table.spectators.size()+table.watchingNext.size());}
   const std::string reason=hint(*selected);
   if(!reason.empty())explanation=explanation.empty()?reason:explanation+"\n"+reason;
   ImGui::PushStyleColor(ImGuiCol_Text,reason.empty()?ToneColor(Tone::Neutral):ToneColor(Tone::Pending));
   ImGui::TextWrapped("%s",explanation.c_str());ImGui::PopStyleColor();
  }
  ImGui::EndChild();
 }
 ImGui::PopStyleColor();ImGui::PopStyleVar();roomBoardFocus_=nav.Focus();
}
const char* ApplicationShell::PlaceExitLabel(const ShellView& v) const {
 const auto place=ExitFromPlace(v);
 return place.table>=0&&menu_.navigation.Focus()=="table-"+std::to_string(place.table)?place.label:"";
}
void ApplicationShell::OpenTableOptions(const ShellView& v,int table) {
 auto& nav=menu_.navigation;
 selectedTable_=table;rulesDirty_=false;rulesRevision_=v.room.tables[table].revision;tableRules_=v.room.tables[table].rules;
 nav.Home();nav.Push("room");nav.Push("room-table");
}
void ApplicationShell::ToggleReady(const ShellView& v,const Submit& submit) {
 const auto* local=Member(v.room,v.room.localMember);
 const auto control=DescribeReady(v,v.room.tables[selectedTable_],local?local->seat:-1);
 if(control.kind==ReadyControl::Unready){
  room::Action request;request.table=static_cast<std::uint8_t>(selectedTable_);
  request.kind=room::ActionKind::Unready;request.seat=local->seat;
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
 }else if(control.kind!=ReadyControl::None)
  Send(control.kind==ReadyControl::Rematch?netplay::CommandKind::Rematch:netplay::CommandKind::Ready,v,submit);
 else if(!control.refusal.empty())error_=control.refusal;
}
// X, Y and View open the fighter, table options and chat; the same button
// again returns to the board.
void ApplicationShell::RoomShortcut(const MenuAction& a,const ShellView& v) {
 auto& nav=menu_.navigation;
 const char* target=a.delta==MenuInput::Fighter?"selection":a.delta==MenuInput::Options?"room-table":"room-chat";
 if(nav.Screen()==target){nav.Home();nav.Push("room");return;}
 if(a.delta==MenuInput::Fighter){
  if(!RoomActionsAvailable(v)||!v.canEditSelection||v.room.localTerminalPending){
   error_=v.room.localTerminalPending?TerminalPendingReason():!v.selectionLockReason.empty()?v.selectionLockReason:
    loc::T("room.change_fighter.unready");
   return;
  }
 }else if(a.delta==MenuInput::Options){
  // The focused table on the board, otherwise the member's own.
  const auto* local=Member(v.room,v.room.localMember);
  const bool onTable=nav.Screen()=="room"&&nav.Focus().compare(0,6,"table-")==0;
  OpenTableOptions(v,onTable?selectedTable_:OptionsTable(v,selectedTable_));
  return;
 }
 nav.Home();nav.Push("room");nav.Push(target);
}
void ApplicationShell::RoomAction(const MenuAction& a,const ShellView& v,const Submit& submit) {
 using namespace room;auto& nav=menu_.navigation;
 const auto sendDelay=[&](netplay::CommandKind kind,int selected){
  if(!RoomActionsAvailable(v) || v.delayLocked ||
     (kind==netplay::CommandKind::CheckConnection && (!v.canProbe || v.probeStatus=="checking"))) {
   error_=RoomWaitReason(v);return false;
  }
  ShellAction request;request.command.kind=kind;request.command.generation=v.session.generation;request.selectedDelay=selected;
  if(!submit(std::move(request))){error_=loc::T("error.queue_failed");return false;}
  error_.clear();return true;
 };
 // A seat chooser's option, exactly as the player saw it.
 if(a.kind==MenuAction::Chosen){
  if(a.id.compare(0,6,"table-"))return;
  selectedTable_=std::stoi(a.id.substr(6));
  if(a.text=="options"){OpenTableOptions(v,selectedTable_);return;}
  const auto* option=FindSeatOption(a.text);
  if(!option)return;
  Action request;request.table=static_cast<std::uint8_t>(selectedTable_);request.kind=option->kind;request.seat=static_cast<std::int8_t>(option->seat);
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
  return;
 }
 if(a.id.compare(0,6,"table-")==0){
  selectedTable_=std::stoi(a.id.substr(6));
  const auto* local=Member(v.room,v.room.localMember);
  // A on your own seat readies; on any other card it opens that table's
  // options, which is looking, not joining: Queue and Watch there keep their
  // own checks.
  if(local&&local->table==selectedTable_&&local->seat>=0)ToggleReady(v,submit);
  else OpenTableOptions(v,selectedTable_);
  return;
 }
 if(a.id=="options"){OpenTableOptions(v,OptionsTable(v,selectedTable_));return;}
 if(a.id=="leave-place"){
  const auto place=ExitFromPlace(v);if(place.table<0)return;
  selectedTable_=place.table;
  if(!place.allowed){error_=place.blocker;return;}
  if(place.kind==ActionKind::Unready){ToggleReady(v,submit);return;}
  Action request;request.table=static_cast<std::uint8_t>(selectedTable_);request.kind=ActionKind::Unqueue;
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
  return;
 }
 if(a.id.compare(0,7,"member-")==0){selectedMember_=std::stoull(a.id.substr(7));nav.Push("room-member");return;}
 if(a.id=="room-members"||a.id=="room-chat"||a.id=="room-admin"||a.id=="room-rules"){nav.Push(a.id);return;}
  if(a.id=="copy"){ImGui::SetClipboardText(v.invitation.c_str());error_.clear();notice_=loc::T("room.invitation_copied");noticeTone_=Tone::Success;noticeUntil_=ImGui::GetTime()+3;return;}
  if(a.id=="replace-room"){ShellAction request;request.command.kind=netplay::CommandKind::ReplaceRoom;request.command.generation=v.session.generation;
   if(!submit(std::move(request)))error_=loc::T("error.queue_failed");
   else{error_.clear();
    // The old invitation dies with the old room epoch. Say so while the row is
    // still on screen, rather than letting friends fail to rejoin silently.
    // This is a caution, not a success.
    notice_=loc::T("room.replacement_opening");
    noticeTone_=Tone::Pending;noticeUntil_=ImGui::GetTime()+8;}
   return;}
  if(a.id=="check-connection"){sendDelay(netplay::CommandKind::CheckConnection,-1);return;}
  if(a.id=="apply-recommendation"){sendDelay(netplay::CommandKind::ApplyDelay,-1);return;}
  if(a.id=="selected-delay"&&a.kind==MenuAction::Adjust){
   const int selected=(std::max)(0,(std::min)(10,v.selectedDelay+a.delta));
   sendDelay(netplay::CommandKind::ApplyDelay,selected);return;
  }
 if(a.id=="leave"){Send(netplay::CommandKind::LeaveRoom,v,submit);return;}
 if(a.id=="mute"){if(muted_.count(selectedMember_))muted_.erase(selectedMember_);else muted_.insert(selectedMember_);return;}
 if(a.id=="compose"){std::snprintf(chat_,sizeof(chat_),"%s",a.text.c_str());return;}
 if(a.id=="rename"){std::snprintf(roomName_,sizeof(roomName_),"%s",a.text.c_str());return;}
 if(a.id=="room-capacity"){roomCapacity_=(std::max)(2,(std::min)(16,roomCapacity_+a.delta));return;}
 if(AdjustRule(tableRules_,a)){rulesDirty_=true;return;}
 Action request;request.table=static_cast<std::uint8_t>(selectedTable_);
 if(a.id=="queue")request.kind=ActionKind::Queue;
 else if(a.id=="unqueue")request.kind=ActionKind::Unqueue;
 else if(a.id=="watch")request.kind=ActionKind::Watch;
  else if(a.id=="unwatch")request.kind=ActionKind::Unwatch;
  else if(a.id=="lock-spectating"){
   const auto* local=Member(v.room,v.room.localMember);
   request.kind=ActionKind::LockSpectating;request.locked=!(local&&local->spectatorLocked);
  }
  else if(a.id=="kick"){request.kind=ActionKind::Kick;request.target=selectedMember_;}
  else if(a.id=="transfer-host"){request.kind=ActionKind::TransferHost;request.target=selectedMember_;}
 else if(a.id=="cancel-result")request.kind=ActionKind::CancelResult;
 else if(a.id=="abandon-result")request.kind=ActionKind::AbortMatch;
 else if(a.id=="apply-rules"){request.kind=ActionKind::SetRules;request.rules=tableRules_;}
 else if(a.id=="apply-name"){request.kind=ActionKind::Rename;request.text=roomName_;}
 else if(a.id=="apply-capacity"){request.kind=ActionKind::SetCapacity;request.capacity=static_cast<std::uint8_t>(roomCapacity_);}
 else if(a.id=="lock"){request.kind=ActionKind::Lock;request.locked=a.delta<0;}
 else if(a.id=="send-chat"){request.kind=ActionKind::Chat;request.text=chat_;}
 else if(a.id=="ready"){ToggleReady(v,submit);return;}
 else return;
 if(SendRoom(std::move(request),v,submit)){
  if(a.id=="send-chat")chat_[0]=0;
  if(a.id=="apply-rules")rulesDirty_=false;
 }else if(error_.empty())error_=loc::T("room.action_rejected");
}
} }
