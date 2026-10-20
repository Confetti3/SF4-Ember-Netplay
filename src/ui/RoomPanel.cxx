#include "ApplicationShell.hxx"
#include "../netplay/UnseatedInputDelay.hxx"
#include "Theme.hxx"
#include "MenuRows.hxx"
#include "RoomFeedback.hxx"
#include "RoomControls.hxx"
#include "RoomCards.hxx"
#include "RoomBoardNavigation.hxx"
#include "LatencyMark.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <utility>

namespace sf4e { namespace ui {
using namespace room_controls;
namespace {
const room::Member* Member(const room::Snapshot& snapshot, room::MemberId id) {
    return room::FindMember(snapshot, id);
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
// "Idle 4 min" for a member who has done nothing in the room for a minute
// or more while waiting (not in a game, not ready, not watching one), else
// empty. A waiting member who said they are in Training is shown as that
// instead: it is why they are quiet.
std::string IdleText(const room::Member& member) {
    const bool waiting = member.status == room::MemberStatus::Idle || member.status == room::MemberStatus::Queued ||
        member.status == room::MemberStatus::Seated;
    if (waiting && member.training) return loc::T("room.in_training");
    if (!waiting || member.idleSeconds < 60) return {};
    const unsigned minutes = member.idleSeconds / 60;
    return minutes < 60 ? loc::Tf("room.idle_minutes", minutes) : loc::Tf("room.idle_hours", minutes / 60, minutes % 60);
}
// A held start is waiting on people, not preparing anything.
const char* PhaseName(const room::Table& table) {
    switch (table.phase) {
    case room::TablePhase::Waiting: return loc::T("room.phase.waiting");
    case room::TablePhase::Ready: return loc::T(table.spectatorHold ? "room.phase.holding" : "room.phase.preparing");
    case room::TablePhase::Playing: return loc::T("room.phase.in_game");
    case room::TablePhase::Paused: return loc::T("room.phase.unresolved");
    case room::TablePhase::Closed: return loc::T("room.phase.closed");
    default: return loc::T("room.phase.open");
    }
}
// The table the options open without a card to say otherwise: your own
// place's, else the last table card you were on.
int OptionsTable(const ShellView& v, int selected) {
    const auto* local = Member(v.room, v.room.localMember);
    return local && local->table >= 0 && local->table < static_cast<int>(v.room.tables.size()) ? local->table : selected;
}
// A draft with something in it besides spaces.
bool HasChatText(const char* text) {
    for(;*text;++text)if(!std::isspace(static_cast<unsigned char>(*text)))return true;
    return false;
}
// A member with no seat and no queue place anywhere gets the chooser from a
// table card. Watching elsewhere is no obstacle: sitting down ends it.
// A room bound to a tournament match seats its fighters itself and refuses
// every seat change, so nobody there is offered the chooser.
bool ChoosesSeat(const room::Snapshot& s) {
    return !s.tournament.Active() && Member(s, s.localMember) && room::PlaceOf(s, s.localMember).kind == room::Place::Kind::None;
}
// A member's status line, wherever it shows: on the board's card, with its
// slot and lock-in, or after the name in a member row, with a local mute. A
// watcher's is its tag instead, and HOST goes only with the host's powers.
std::string MemberStatusText(const room::Snapshot& s, const room::Member& m, bool muted, bool board) {
    if (room::IsWatcher(s, m.id)) return loc::Tf(muted ? "room.member_status_muted" : "room.member_status", WatcherTag(s, m));
    const bool host = m.id == room::ActingHost(s);
    if (board)
        return loc::Tf(m.table >= 0 ? (host ? "room.board_status_host_slot" : "room.board_status_slot") : (host ? "room.board_status_host" : "room.board_status"),
            StatusName(m.status), m.table + 1) + (m.spectatorLocked ? loc::T("room.suffix_locked_in") : "");
    return loc::Tf(host ? (muted ? "room.member_status_host_muted" : "room.member_status_host") : (muted ? "room.member_status_muted" : "room.member_status"),
        StatusName(m.status));
}
// The same local pick is editable before and after taking a seat. Select
// opens its page; Left and Right step only the Ultra and color in place.
void SelectionRows(std::vector<MenuEntry>& rows,const ShellView& v) {
 const bool canChange=RoomActionsAvailable(v)&&v.canEditSelection&&!v.room.localTerminalPending;
 rows.push_back(Row("selection",loc::T("selection.fighter"),canChange?
  std::string(loc::T("room.change_fighter.detail"))+"\n"+v.selectionSummary:SelectionBlocker(v),canChange));
 rows.back().value=v.fighterName;
 const auto stepRow=[&](const char* id,const char* label,const std::string& value,const char* detail,bool steps){
  rows.push_back(Value(id,label,value,canChange?detail:SelectionBlocker(v),canChange));
  rows.back().adjustable=canChange&&steps;rows.back().opens=true;
 };
 stepRow("ultra",loc::T("selection.ultra_combo"),v.ultraName,loc::T("selection.ultra_row.detail"),v.ultraSteps);
 stepRow("appearance",loc::T("selection.appearance"),v.appearanceName,loc::T("room.appearance.detail"),v.colorSteps);
 rows.push_back(Value("fighter-options",loc::T("room.fighter_options"),v.fighterOptionsName,
  canChange?std::string(loc::T("selection.additional_options.detail")):SelectionBlocker(v),canChange));
 rows.back().adjustable=false;rows.back().opens=true;
}
}
bool ApplicationShell::SendRoom(room::Action action, const ShellView& view, const Submit& submit) {
    // Button disabled state is a rendering affordance; keyboard/controller
    // activation can still reach an item ID. Recheck the room authority
    // boundary at dispatch so a lost or closed control stream never queues a
    // room mutation.
    if (!RoomTakesNow(view, action.kind, action.table) || !view.room.roomEpoch || !view.room.localMember) {
        Refuse(RoomWaitReason(view), [kind = action.kind, table = action.table](const ShellView& now) {
            return !RoomTakesNow(now, kind, table) || !now.room.roomEpoch || !now.room.localMember;
        });
        return false;
    }
    if (action.table >= view.room.tables.size()) return false;
    // An Unwatch of the game this PC watches is that game's leave, which the
    // runtime scopes to the watched game when it runs.
    if (action.kind == room::ActionKind::Unwatch && LeavesWatchedGame(view, action.table)) return LeaveWatchedGame(view, submit);
    const auto mine = room::PlaceOf(view.room, view.room.localMember);
    const auto& table = view.room.tables[action.table];
    if (TerminalFenced(view.room, action.kind, action.table)) {
        Refuse(TerminalPendingReason(), [kind = action.kind, index = action.table](const ShellView& now) {
            return TerminalFenced(now.room, kind, index);
        });
        return false;
    }
    const bool localSeated = mine.kind == room::Place::Kind::Seat && mine.table == static_cast<int>(action.table);
    if ((action.kind == room::ActionKind::Queue || action.kind == room::ActionKind::Watch ||
        action.kind == room::ActionKind::Unwatch) && localSeated) return false;
    if (action.kind == room::ActionKind::AbortMatch && AbandonableAt(view, table) == AbandonableGame::None) return false;
    if (action.kind == room::ActionKind::Unqueue && localSeated) {
        // The same rule the board and table departure controls read.
        const auto hold = room::LeavingSeatHold(table, mine.seat, GameLive(view), PostMatch(view));
        if (hold != room::SeatHold::None) { Refuse(loc::T(LeaveBlockerKey(hold)), LeaveBlocked); return false; }
    }
    action.protocolVersion = room::ProtocolVersion;
    action.roomEpoch = view.room.roomEpoch;
    action.revision = view.room.revision;
    if (action.table < view.room.tables.size()) {
        action.tableRevision = table.revision;
		const bool generationScoped = action.kind == room::ActionKind::RecordResult ||
			action.kind == room::ActionKind::MatchFinished || action.kind == room::ActionKind::CancelResult ||
			action.kind == room::ActionKind::AbortMatch ||
			(action.kind == room::ActionKind::Unwatch &&
				(table.phase == room::TablePhase::Playing || table.phase == room::TablePhase::Paused));
		if (!action.matchGeneration && generationScoped)
			action.matchGeneration = table.matchGeneration;
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
 std::vector<MenuEntry> rows;
  const bool mutableRoom=RoomActionsAvailable(v);
  const bool host=mutableRoom&&s.localMember&&s.localMember==room::ActingHost(s);
  // A room bound to a tournament match seats its two fighters at table 0 by
  // itself and keeps everyone else out; the room refuses seat, watch, rules
  // and kick changes there, so they are not offered.
  const bool tournamentRoom=s.tournament.Active();
  const bool boundTable=tournamentRoom&&selectedTable_==room::TournamentTable;
 const auto* local=Member(s,s.localMember);
 const auto screen=nav.Screen();
 // The personal delay editor follows a seat granted while the page is open.
 if(screen=="room-match")selectedTable_=OptionsTable(v,selectedTable_);
 const auto& t=s.tables[selectedTable_];
 // A question about leaving a seat belongs to the board it was asked on.
 if(screen!="room"){leaveAsk_=-1;leaveAsked_=false;}
 // A new game at the same table closes a dialog about the last one; moving to
 // another table (a click opens its chooser a frame before this catches up)
 // does not. Text being typed is not about a game, and only the board and the
 // table's options hold dialogs about one.
 if(generationTable_!=selectedTable_){generationTable_=selectedTable_;tableGeneration_=t.matchGeneration;}
 else if(tableGeneration_!=t.matchGeneration){
  tableGeneration_=t.matchGeneration;
  if(!nav.Editing()&&(screen=="room"||screen=="room-table"))nav.Cancel();
 }
 const bool active=t.phase==TablePhase::Ready||t.phase==TablePhase::Playing||t.phase==TablePhase::Paused;
 const auto place=room::PlaceOf(s,s.localMember);
 const bool seated=place.kind==room::Place::Kind::Seat&&place.table==selectedTable_;
 // A seat or queue place at another table blocks joining this one. Watching
 // elsewhere does not, because Queue and Watch both end it (the authority's
 // StopWatching); the board's seat chooser applies the same rule.
 const bool elsewhere=place.kind!=room::Place::Kind::None&&place.table!=selectedTable_;
 const bool queued=std::find(t.queue.begin(),t.queue.end(),s.localMember)!=t.queue.end();
 // A queued member is listed as a spectator of the game it waits out without
 // having chosen to watch it.
 const bool watching=room::WatchesByChoice(t,s.localMember);
  if(screen=="room"){
  if(s.roomEpoch){
   const auto mine=room::PlaceOf(s,s.localMember);
   for(const auto& table:s.tables){
    const auto occupied=(table.p1?1:0)+(table.p2?1:0);
    std::string detail=loc::Tf("room.table_detail",Name(s,table.p1),Name(s,table.p2),PhaseName(table),
     table.queue.size(),table.spectators.size()+table.watchingNext.size(),local?StatusName(local->status):loc::T("room.connecting"));
    rows.push_back(Row("table-"+std::to_string(table.id),loc::Tf("room.table_occupancy",table.id+1,occupied),detail));
    rows.back().detailText=DetailText::Name;
    const bool ownTable=mine.table==static_cast<int>(table.id);
    if(ChoosesSeat(s))rows.back().choices=SeatChoices(v,table);
    else if(mine.kind==Place::Kind::Seat&&ownTable){
     const auto control=DescribeReady(v,table,mine.seat);
     const auto exit=ExitFromPlace(v);
     const auto selectionBlocker=SelectionBlocker(v);
     rows.back().hint=loc::T("room.actions");
     rows.back().choices={
      {control.kind==ReadyControl::Unready?"unready":"ready",std::string(loc::T(control.kind==ReadyControl::Unready?"room.unready.short":"room.status.ready")),control.detail,control.kind!=ReadyControl::None},
      {"selection",loc::T("selection.fighter"),selectionBlocker.empty()?std::string(loc::T("room.change_fighter.detail")):selectionBlocker,selectionBlocker.empty()}};
     if(exit.table>=0)rows.back().choices.push_back({exit.unready?"cancel-start":"leave-place",exit.unready?loc::T("common.cancel"):exit.label,DepartureDetail(v),mutableRoom&&exit.allowed});
     rows.back().choices.push_back({"options",loc::T("room.more_actions"),loc::T("room.table_options")});
     if(table.spectatorHold&&control.kind==ReadyControl::Unready)rows.back().detail=control.detail;
     const auto cost=CostOfLeavingSeat(table);
     if(leaveAsk_==static_cast<int>(table.id)&&cost)rows.back().choices={
      {"stay",loc::T("common.cancel"),loc::T("room.leave_seat.keep")},
      {"leave-seat",loc::T("room.leave_seat"),LeavingCostText(cost,table)}};
    }
    else if(mine.kind==Place::Kind::Queue&&ownTable){
     rows.back().hint=loc::T("room.actions");
     rows.back().choices={{"options",loc::T("room.actions"),loc::T("room.table_options")},
      {"leave-place",loc::T("room.leave_queue"),DepartureDetail(v),mutableRoom}};
    }
   }
   if(leaveAsk_>=0){
    if(!leaveAsked_){if(nav.Ask("table-"+std::to_string(leaveAsk_),rows))leaveAsked_=true;else leaveAsk_=-1;}
    else if(!nav.Choosing()){leaveAsk_=-1;leaveAsked_=false;}
   }
   for(const auto& m:s.members){rows.push_back(Row("member-"+std::to_string(m.id),loc::Tf(m.id==s.localMember?"room.member_you":"room.member",m.name),MemberStatusText(s,m,false,false)));
    rows.back().userText=true;}
   // Over a Practice match it fights in, a fighter can end the game.
   if(mine.kind==Place::Kind::Seat&&AbandonableAt(v,s.tables[mine.table])==AbandonableGame::Practice)
    rows.push_back(ConfirmRow("end-practice",loc::T("room.end_practice"),loc::T("room.end_practice.detail"),mutableRoom));
   // Over a game it only watches, a spectator can leave it; once the room has
   // finished that game, the rest of its stream, which asks nothing of the room.
   if(const auto leave=netplay::LeaveOfWatchedGame(s,v.watchedGame);leave!=netplay::WatchedLeave::None){
    const bool leaveGame=leave==netplay::WatchedLeave::LeaveGame,viewOnly=leave==netplay::WatchedLeave::EndView;
    rows.push_back(ConfirmRow("stop-watching",loc::T(leaveGame?"room.leave_game":"room.stop_watching"),
     loc::T(viewOnly?"room.stop_watching.finished":leaveGame?"room.leave_game.detail":"room.stop_watching.detail"),
     viewOnly||RoomTakesNow(v,ActionKind::Unwatch,static_cast<std::size_t>(v.watchedGame.table))));
   }
   // Seated fighters use their table's actions. Keep fighter selection here
   // before taking a seat, when the table has no Fighter choice to duplicate.
   if(mine.kind!=Place::Kind::Seat)rows.push_back(Row("selection",loc::T("home.fighter_select"),SelectionBlocker(v).empty()?
    loc::T("room.change_fighter.detail"):SelectionBlocker(v),SelectionBlocker(v).empty()));
   rows.push_back(Row("room-members",loc::T("room.members"),loc::Tf("room.member_count",s.members.size(),s.capacity)));
   const unsigned unread=transcript_.Unread(muted_);
   rows.push_back(Row("room-chat",loc::T("chat.history"),(unread?loc::Tf("room.chat.unread",unread)+" ":std::string())+loc::T("room.chat.detail")));
   if(unread)rows.back().value=std::to_string(unread);
   // Every controller can reach these actions without a device-specific shortcut.
   rows.push_back(Row("settings",loc::T("home.settings"),loc::T("home.settings_detail")));
  }else{
   rows.push_back(Row("room-status",loc::T("room.connection_status"),loc::T(v.session.room==netplay::RoomState::Opening?"room.opening":"room.waiting_state"),false));
  }
  // A tournament room admits only its two fighters, who reach it through the
  // tournament service, so there is no invitation to hand out. A public room
  // admits by the service's ticket, so its invitation would admit nobody.
  if(!tournamentRoom&&!v.tournament.MatchRoomActive())rows.push_back(Row("room-invite",loc::T("room.invite"),loc::T("room.invite.detail")));
  if(s.roomEpoch)rows.push_back(Row("room-training",loc::T("room.training"),loc::T(v.canTrain?"room.training.detail":"room.training.unavailable"),v.canTrain));
  rows.push_back(ConfirmRow("leave",loc::T(v.session.room==netplay::RoomState::Closing?"room.leaving":"room.leave"),
   LeaveRoomDetail(v),v.session.room!=netplay::RoomState::Closing));
  // A public room has no replacement: when its host is gone the room closes.
  if(v.session.recovery==netplay::Recovery::ReplacementOffered&&!s.serverOwned)
   rows.push_back(ConfirmRow("replace-room",loc::T("room.replace"),loc::T(v.canReplaceRoom?"room.replace.detail":"room.replace.waiting"),v.canReplaceRoom));
  }else if(screen=="room-invite"){
  if(!tournamentRoom&&!s.serverOwned&&!v.tournament.MatchRoomActive()){
   rows.push_back(Row("copy",loc::T("room.copy_invitation"),loc::T("room.copy_invitation.detail"),!v.invitation.empty()));
   // Once made, the detail shows the link itself so it can be read out.
   rows.push_back(Row("copy-short",loc::T("room.copy_short_invitation"),v.shortInvitation.empty()?std::string(loc::T("room.copy_short_invitation.detail")):
    v.shortInvitation.substr(v.shortInvitation.find("//")==std::string::npos?0:v.shortInvitation.find("//")+2),!v.invitation.empty()));
  }
  // A public room is shared by its page link, which opens it in Ember from a browser or Discord.
  if(s.serverOwned){const auto link=publicRooms_.RoomLink();if(!link.empty())rows.push_back(Row("copy-room-link",loc::T("public.copy_link"),loc::Tf("public.copy_link_detail",link)));}
 }else if(screen=="room-table"||screen=="room-match"||screen=="room-rules"){
   const std::string reason=!mutableRoom?RoomWaitReason(v):
    loc::T(elsewhere?"room.leave_current_table":"room.choose_action");
  // The table's rules: the host edits them in place and applies them in one
  // press (applying clears Ready), everyone else reads them on one line.
  const auto rulesRows=[&]{
   if(!rulesDirty_&&rulesRevision_!=t.revision){tableRules_=t.rules;rulesRevision_=t.revision;}
   if(!host||boundTable){
    std::string summary=loc::Tf(s.practice?"room.rules_summary_practice":"room.rules_summary",static_cast<int>(t.rules.roundCount),static_cast<int>(room::MatchRoundTime(s,t)),
     loc::T(t.rules.editionSelect?"common.on":"common.off"));
    if(s.delayPolicy==room::DelayPolicy::Own)summary=loc::Tf("room.rules_summary_own_delay",summary);
    rows.push_back(ReadOnlyValue("rules",loc::T("room.table_rules"),summary,loc::T(boundTable?"room.rules.tournament":"room.rules.host_only")));
    rows.push_back(ReadOnlyValue("set-rules",loc::T("room.table_set"),SetSummaryText(t.rules),
     t.rules.format==room::SetFormat::Unlimited?loc::T("rules.set_length.detail"):RotationDetail(t.rules.rotation)));
    return;
   }
   RuleRows(rows,tableRules_,!active,loc::T(active?"room.rules.finish_game":"room.rules.apply_note"),s.practice);
   if(rulesDirty_)rows.push_back(Row("apply-rules",loc::T("room.apply_rules"),loc::T("room.apply_rules.detail"),!active));
   // The host edits the rules in place, so the summary above is not shown to them.
   if(s.delayPolicy==room::DelayPolicy::Own)rows.push_back(ReadOnlyValue("delay-policy",loc::T("room.delay_policy"),loc::T("room.delay_policy.own"),loc::T("room.delay_policy.detail")));
  };
  if(screen=="room-rules"){
   rows.push_back(Value("rules-table",loc::T("screen.table"),std::to_string(selectedTable_+1),loc::T("room.rules_target"),!rulesDirty_&&!boundTable));
   rulesRows();
  }else if(seated){
   const bool ready=room::ReadyCancellable(t,place.seat);
    const auto readyControl=DescribeReady(v,t,place.seat);
    // A Training call's window or the room's Ready timeout: the row says how
    // long there is to ready.
    const auto deadline=DescribeReadyDeadline(v,t);
    const auto timeout=deadline.seat==place.seat?deadline.text:std::string();
   if(screen=="room-table"){
    rows.push_back(Row("ready",readyControl.label,timeout.empty()?readyControl.detail:timeout+"\n"+readyControl.detail,readyControl.kind!=ReadyControl::None));
    if(t.spectatorHold||!timeout.empty())rows.back().detailText=DetailText::Name;
    SelectionRows(rows,v);
    const bool canChange=mutableRoom&&v.canEditSelection&&!s.localTerminalPending;
    const bool stageOwner=v.localSlot==0;
    rows.push_back(Value("stage",loc::T("selection.stage"),v.stageName,
     !canChange?SelectionBlocker(v):loc::T(stageOwner?"selection.p1_stage":"selection.only_p1_stage"),canChange&&stageOwner));
    rows.back().adjustable=false;rows.back().opens=true;
    rows.push_back(Row("room-match",loc::T("settings.input_delay"),loc::T("settings.delay_saved_detail")));
   }
   if(screen=="room-match"){
    // One delay row: Left and Right choose it, Select takes the recommendation.
    // Auto sits before the smallest delay. It shows a number once the check
    // has measured this opponent, or once Ready has locked one.
    const bool autoDelay=v.preferences.autoInputDelay;
    const bool delayEditable=mutableRoom&&!active&&!v.delayLocked;
    const int selectedDelay=(std::max)(MinimumInputDelay,(std::min)(MaximumInputDelay,v.selectedDelay));
    const bool recommended=v.recommendedDelay>=0&&v.recommendedDelay<=MaximumInputDelay;
    const auto check=DescribeConnectionCheck(v);
    const bool opponentReady=v.opponentDelay>=0&&v.opponentDelay<=InputDelayWireCeiling;
    std::string delayDetail=loc::Tf("room.input_delay.recommended",check.value)+"\n"+loc::T("settings.delay_saved_detail");
    // An Own room plays each fighter at their own delay, so there is no match value to show.
    if(t.p1&&t.p2)delayDetail+="\n"+(s.delayPolicy==room::DelayPolicy::Own?loc::Tf("room.input_delay.own",FramesText(selectedDelay)):loc::Tf("room.input_delay.match",opponentReady?
     FramesText(room::MatchDelay(selectedDelay,v.opponentDelay)):loc::Tf(selectedDelay==1?"room.match_delay.at_least_one":"room.match_delay.at_least",selectedDelay)));
    if(!delayEditable)delayDetail+="\n"+(v.delayLocked?std::string(loc::T("room.selected_delay.locked")):reason);
    else{
     delayDetail+="\n"+(autoDelay?loc::Tf("room.input_delay.auto.detail",AutoInputDelayMinimum,AutoInputDelayMaximum):loc::Tf("room.selected_delay.detail",MinimumInputDelay,MaximumInputDelay));
     if(!recommended&&!autoDelay)delayDetail+="\n"+std::string(loc::T("room.apply_recommendation.check_first"));
    }
    const std::string delayFrames=FramesText(selectedDelay);
    const std::string delayValue=!autoDelay?delayFrames:
     v.autoDelayMeasured||v.delayLocked?loc::Tf("room.input_delay.auto",delayFrames):std::string(loc::T("settings.input_delay.auto"));
    rows.push_back(Value("input-delay",loc::T("room.input_delay"),delayValue,delayDetail,delayEditable));
    rows.back().opens=!autoDelay&&v.canApplyDelay&&recommended&&!check.checking;
    if(rows.back().opens)rows.back().hint=loc::T("room.apply_recommendation");
    rows.push_back(Row("check-connection",check.action,check.checking?check.detail:
     !mutableRoom?reason:!t.p1||!t.p2?loc::T("room.check_connection.two_players"):
     ready?loc::T("room.check_connection.unready"):
     !delayEditable?loc::T("room.check_connection.finish_match"):check.detail,
     v.canProbe&&delayEditable&&!check.checking));
    if(check.namesPlayer)rows.back().detailText=DetailText::Name;
   }else{
    const auto leave=ExitFromPlace(v);
    const bool leaves=leave.allowed&&!leave.unready;
    const auto leaveDetail=DepartureDetail(v,false);
    if(!boundTable){
     rows.push_back(Row("unqueue",loc::T("room.leave_seat"),leaveDetail,mutableRoom&&leaves));
     rows.back().confirm=leaves&&bool(leave.cost);
    }
    // The authority accepts AbortMatch from either fighter. Offer it only once
    // the result is Paused, so a fighter can never cut a live game short.
    if(AbandonableAt(v,t)==AbandonableGame::Unresolved)rows.push_back(ConfirmRow("abandon-result",loc::T("room.abandon_result"),
     loc::T("room.abandon_result.detail"),mutableRoom));
   }
   }else if(screen=="room-table"){
    SelectionRows(rows,v);
    // Enablement asks the same fence SendRoom dispatches through, so the row
    // and the dispatch guard cannot drift apart.
    const auto tableRow=[&](room::ActionKind kind,const char* id,const char* label){
     const bool fenced=TerminalFenced(s,kind,selectedTable_);
     rows.push_back(Row(id,loc::T(label),(fenced?TerminalPendingReason():reason)+WatchingNote(v,kind),mutableRoom&&!elsewhere&&!fenced));
    };
    if(!boundTable)tableRow(queued?room::ActionKind::Unqueue:room::ActionKind::Queue,queued?"unqueue":"queue",
     queued?"room.leave_queue":"room.join_queue");
    // A queued member has no watch choice: Watch is refused them, and the game
    // they wait out is theirs to watch until they are seated or leave the queue.
    if(!queued&&!boundTable)tableRow(watching?room::ActionKind::Unwatch:room::ActionKind::Watch,watching?"unwatch":"watch",
     watching?"room.stop_watching":"room.watch_next");
    // A watcher's lock-in is never fenced: the one still leaving the last
    // game is exactly who it is for.
    if(room::WatchesByChoice(t,s.localMember)){
     const bool lockedIn=local&&local->spectatorLocked;
     rows.push_back(Row("lock-spectating",loc::T(lockedIn?"room.unlock_spectating":"room.lock_spectating"),
      mutableRoom?loc::Tf(lockedIn?"room.unlock_spectating.detail":"room.lock_spectating.detail",room::SpectatorStartHoldMs/1000):reason,mutableRoom));
    }
  }else {
    const bool can=netplay::CanSaveUnseatedInputDelay(v.session,s);
    const auto& preferences=v.preferences;
    const std::string detail=std::string(loc::T("settings.delay_future_detail"))+"\n"+
     (preferences.autoInputDelay?loc::Tf("room.input_delay.auto.detail",AutoInputDelayMinimum,AutoInputDelayMaximum):
      loc::Tf("room.selected_delay.detail",MinimumInputDelay,MaximumInputDelay));
    rows.push_back(Value("input-delay",loc::T("settings.input_delay"),preferences.autoInputDelay?
     std::string(loc::T("settings.input_delay.auto")):FramesText(preferences.inputDelay),detail,can));
    rows.push_back(InfoRow("delay-policy",loc::T("room.delay_policy"),
     loc::T(s.delayPolicy==room::DelayPolicy::Own?"room.delay_policy.own":"room.delay_policy.shared"),loc::T("room.delay_policy.detail")));
    rows.push_back(InfoRow("connection-seat",loc::T("connection.check"),"",loc::T("settings.delay_check_seated")));
  }
  if(screen=="room-table"&&t.phase==TablePhase::Paused)rows.push_back(ConfirmRow("cancel-result",loc::T("room.cancel_unresolved"),loc::T("room.cancel_unresolved.detail"),host));
  // A table can be left in Playing when neither fighter's finish report
  // reached the room (both clients lost control at battle close). The
  // authority accepts a host cancel in that phase; offer it to a host who is
  // not one of the fighters, so a live game can never be cut short by its
  // own participant from this row. Only a game that has run without a result
  // for StaleGameSeconds is offered; before that the row says the game is live.
  else if(screen=="room-table"&&t.phase==TablePhase::Playing&&host&&!seated&&!t.resultPending){
   const bool stale=GameIsStale(selectedTable_);
   rows.push_back(ConfirmRow("cancel-result",loc::T("room.cancel_stuck"),stale?std::string(loc::T("room.cancel_stuck.detail")):
    loc::Tf("room.cancel_stuck.in_progress",room::StaleGameSeconds/60),stale));
  }
 }else if(screen=="room-members"){
  for(const auto& m:s.members){
   rows.push_back(Row("member-"+std::to_string(m.id),loc::Tf(m.id==s.localMember?"room.member_you":"room.member",m.name),MemberStatusText(s,m,muted_.count(m.id)>0,false)));
   rows.back().value=NetworkLinkName(m.link);rows.back().userText=true;
  }
  }else if(screen=="room-member"){
   const auto* m=Member(s,selectedMember_);const bool other=m&&m->id!=s.localMember;
   rows.push_back(Row("mute",loc::T(muted_.count(selectedMember_)?"room.unmute_member":"room.mute_member"),loc::T(m?"room.mute_member.detail":"room.member_left.detail"),other));
   // Only a member who chose to show the room their profile has one to open.
   const bool hasSteam=m&&m->steamId.Present();
   rows.push_back(Row("steam-profile",loc::T("room.steam_profile"),
    loc::T(!m?"room.member_left.detail":hasSteam?"room.steam_profile.detail":"room.steam_profile.none"),hasSteam));
   if(!tournamentRoom){
    rows.push_back(ConfirmRow("kick",loc::T("room.kick_member"),m?loc::Tf("room.kick_member.detail",m->name):loc::T("room.member_left.detail"),host&&other));
    rows.back().detailText=DetailText::Name;
   }
   rows.push_back(ConfirmRow("transfer-host",loc::T("room.transfer_host"),m?loc::Tf("room.transfer_host.detail",m->name):loc::T("room.member_left.detail"),
    host&&other&&!room::IsWatcher(s,selectedMember_)));
   rows.back().detailText=DetailText::Name;
 }else if(screen=="room-chat"){
  // The one row: the message box is drawn by DrawChatScreen, and Enter in it is this row's Select. It is
  // there to send once there is something to send, and not again while that same text is on its way.
  rows.push_back(Row("compose",loc::T("room.compose_message"),loc::T("chat.detail"),
   ChatTypable(v)&&HasChatText(chat_)&&!queuedChat_&&!(ChatInFlight(ImGui::GetTime())&&pendingChat_->text==chat_)));
  rows.back().value=chat_;rows.back().hint=loc::T("chat.send");
 }else if(screen=="room-admin"){
  rows.push_back(TextRow("rename",loc::T("room.name"),roomName_,64,host));
  rows.push_back(Value("room-capacity",loc::T("room.capacity"),std::to_string(roomCapacity_),loc::T("room.capacity.detail"),host));
  rows.push_back(Value("lock",loc::T("room.admission"),loc::T(s.locked?"room.locked":"room.open"),loc::T("room.admission.detail"),host));
  rows.push_back(Row("apply-name",loc::T("room.apply_name"),loc::T("room.apply_name.detail"),host&&roomName_[0]));
  rows.push_back(Row("apply-capacity",loc::T("room.apply_capacity"),loc::T("room.apply_capacity.detail"),host&&roomCapacity_>=static_cast<int>(s.members.size())));
 }
 if(!active) {
  for(auto& row:rows)if(row.id=="selection"||row.id=="ultra"||row.id=="appearance"||row.id=="stage"||row.id=="fighter-options"||row.id=="check-connection"||
   row.id=="input-delay"||row.id=="queue"||row.id=="watch") {
    // Per table: a cached explanation never shows on another table's row.
    const auto key=screen+"/"+std::to_string(selectedTable_)+"/"+row.id;
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
 if(screen=="room"){
  if(s.roomEpoch&&ChatGateOf(v)!=ChatGate::ReadOnly){rows.push_back(Row("inline-chat",loc::T("room.compose_message"),loc::T("chat.inline_detail"),ChatTypable(v)));rows.back().value=chat_;}
  LinkRoomBoard(rows,roomBoardColumns_);
 }
 return rows;
}
void ApplicationShell::DrawRoomBoard(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height,
                                     const MenuVisualFeedback& feedback) {
 boardDrawn_=true;
 const float s=Scale(),gap=12*s;
 const auto focus=nav.Focus();const bool changed=focus!=roomBoardFocus_;
 const unsigned unread=transcript_.Unread(muted_);
 // The board renders in place of the menu list, so it uses the same smoothed
 // verdict and the same wording. Presentation only: nav.Choose still gates on
 // the live entry and SendRoom still re-checks room authority at dispatch.
 const auto shown=[&](const MenuEntry& e){return feedback.Enabled(e);};
 const auto hint=[&](const MenuEntry& e){return feedback.Enabled(e)?"":loc::T(feedback.Pending(e)?"room.updating":"common.unavailable");};
 const auto origin=ImGui::GetCursorScreenPos();
 if(focus.compare(0,6,"table-")==0)selectedTable_=std::stoi(focus.substr(6));
 // Back always navigates. Delete and the explicit leave button use the same
 // guarded departure path; Delete keeps its existing own-card scope.
 const auto place=ExitFromPlace(v);
 const bool onOwnPlace=OnOwnPlace(place,focus);
 const bool leaveKey=action.kind==MenuAction::Shortcut&&action.delta==MenuInput::Leave;
 if(onOwnPlace&&leaveKey)action={MenuAction::Activate,"leave-place"};
 std::string elided;
 const auto text=[&](ImVec2 p,float width,const std::string& value,float size,ImU32 color,bool centred=false){DrawCardText(elided,p,width,value,size,color,centred);};
 // Flushed once per row, while the row's button is still the hovered item.
 const auto tip=[&]{FlushCardTip(elided);};
 const auto press=[&](const MenuEntry& e,ImVec2 size){
  if(DrawCardButton(e.id,size,e.id==focus,changed,nav.Choosing())&&!nav.Asking()&&!nav.Editing()){nav.Focus(e.id,rows);action=nav.Choose(rows);}
 };
 const auto fighter=[&](const room::Member* m){return m?(m->id==v.room.localMember?v.selectedFighter:m->fighter):-1;};
 // The highlighted option of the entry's open choice, in full; empty otherwise.
 const auto choiceDetail=[&](const MenuEntry& e)->std::string{
  return nav.Choosing()&&nav.DialogId()==e.id&&nav.ChoiceIndex()<e.choices.size()?e.choices[nav.ChoiceIndex()].detail:std::string();
 };
 // A card's body press does what Select does on it. Its strip options are the card's callers' to answer.
 const auto pressBody=[&](const MenuEntry& e,const CardInteraction& hit){
  if(hit.kind==CardInteraction::Kind::Pressed&&hit.option<0&&!nav.Asking()&&!nav.Editing()){nav.Focus(e.id,rows);action=nav.Choose(rows);}
 };
 const auto tableCard=[&](const MenuEntry& e,float h){
  const auto& t=v.room.tables[std::stoi(e.id.substr(6))];
  // The seat chooser, or your focused seat's ready and departure controls.
  const bool choosing=nav.Choosing()&&nav.DialogId()==e.id&&!e.choices.empty();
  const bool ownPlace=!nav.Asking()&&onOwnPlace&&e.id==focus;
  TableCardView card;card.id=e.id;card.slot=t.id;card.height=h;
  card.focused=e.id==focus;card.focusChanged=changed;card.disabled=nav.Choosing();
  if(choosing){
   card.choosing=true;card.chosen=static_cast<int>(nav.ChoiceIndex());
   for(const auto& c:e.choices){const auto* option=FindSeatOption(c.id);card.options.push_back({c.label,c.enabled,option?option->seat:-1});}
  }
  else if(ownPlace)card.options={{loc::T("room.actions"),true}};
  const auto banner=DescribeTableBanner(v,t);
  card.phase=PhaseName(t);card.banner=banner.text;
  // A full pair shows its running win count where "VS" would sit.
  card.middle=t.p1&&t.p2?SetScoreText(t.score):"VS";
  const room::MemberId ids[]={t.p1,t.p2};
  for(int side=0;side<2;++side){
   const auto* m=Member(v.room,ids[side]);if(!m)continue;
   auto& seat=card.seats[side];
   seat.occupied=true;seat.name=m->name;seat.fighter=fighter(m);seat.link=m->link;seat.country=m->country;seat.marked=banner.seat==side;
   seat.latency=LatencyMarkFor(v.memberLatency,*m,v.room.localMember);
   // A readied fighter wears a badge on the portrait until the game starts.
   seat.ready=t.ready[side]&&(t.phase==room::TablePhase::Waiting||t.phase==room::TablePhase::Ready);
   const auto* f=selection::FindFighter(seat.fighter);
   seat.caption=f?f->name:loc::T("room.fighter_not_shared");
   if(m->id==v.room.localMember)seat.caption+=loc::T("room.suffix_you");
   if(t.streakHolder==m->id&&t.streak>=2)seat.streak=loc::Tf("room.streak",t.streak);
   if(!t.ready[side])seat.idle=IdleText(*m);
  }
  // A first-to-N table names its set and rotation where the round settings
  // would be; those stay on the table's rules rows.
  const auto watching=t.spectators.size()+t.watchingNext.size();
  card.footer=(v.room.practice?std::string(loc::T("room.table_footer_practice"))+"     ":std::string())+
   (v.room.delayPolicy==room::DelayPolicy::Own?std::string(loc::T("room.table_footer_own_delay"))+"     ":std::string())+(t.rules.format==room::SetFormat::Unlimited
   ?loc::Tf("room.table_footer",t.rules.roundCount,room::MatchRoundTime(v.room,t),t.queue.size(),watching)
   :loc::Tf("room.table_footer_set",SetLengthText(t.rules.format),RotationText(t.rules.rotation),t.queue.size(),watching));
  const auto hit=DrawTableCard(card);
  pressBody(e,hit);
  const bool optionPressed=hit.kind==CardInteraction::Kind::Pressed&&hit.option>=0;
  if(choosing){
   // The chosen side follows the mouse, but only a moving one takes the choice.
   if(hit.option>=0&&hit.pointerMoved)nav.ChoiceIndex(static_cast<std::size_t>(hit.option));
   if(optionPressed)action=nav.Pick(static_cast<std::size_t>(hit.option),rows);
  }else if(ownPlace&&optionPressed){nav.Focus(e.id,rows);action=nav.Choose(rows);}
 };
 const auto memberCard=[&](const MenuEntry& e){
  const auto* m=Member(v.room,std::stoull(e.id.substr(7)));if(!m)return;
  MemberCardView card;card.id=e.id;card.name=m->name;card.label=e.label;card.link=m->link;card.country=m->country;
  card.latency=LatencyMarkFor(v.memberLatency,*m,v.room.localMember);
  card.mainFighter=m->id==v.room.localMember?v.preferences.mainFighter:m->mainFighter;
  card.focused=e.id==focus;card.focusChanged=changed;card.disabled=nav.Choosing();
  const auto status=MemberStatusText(v.room,*m,false,true);
  const auto idle=IdleText(*m);
  card.status=idle.empty()?status:status+" \xC2\xB7 "+idle;
  pressBody(e,DrawMemberCard(card));
 };
 const auto button=[&](const MenuEntry& e,float width){
  const auto p=ImGui::GetCursorScreenPos();press(e,ImVec2(width,40*s));
  ReportMenuCard(e.id.c_str(),p,ImVec2(p.x+width,p.y+40*s));
  // The unread count rides the Chat row's right edge, and the label leaves room for it.
  const float badge=e.id=="room-chat"&&unread?UnreadBadgeWidth(unread)+8*s:0;
  if(badge)DrawUnreadBadge(p.x+width-8*s,p.y+11*s,unread);
  // Our own action labels must fit; only player-supplied text may elide.
  ReportMenuText(("board-"+e.id).c_str(),16*s,40*s,
   ImGui::GetFont()->CalcTextSizeA(16*s,FLT_MAX,0,e.label.c_str()).x,width-24*s-badge);
  text(ImVec2(p.x+12*s,p.y+11*s),width-24*s-badge,e.label,16*s,shown(e)?palette::Ivory:palette::Muted);
  tip();
 };
 const auto parts=PartitionRoomBoard(rows);
 const auto layout=MeasureRoomBoard(parts,ImGui::GetContentRegionAvail().x,height);
 // Links use the last measured layout; a resize may affect them for one frame.
 roomBoardColumns_=layout.wide?layout.columns:1;
 ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(6*s,6*s));
 ImGui::PushStyleColor(ImGuiCol_ChildBg,BoardPanelColor(v.SeeThrough()));
 if(layout.wide){
  // The battle slots take the whole height on the left. Members, chat and the
  // room's actions share the right. Their explanation stays below the actions.
  ImGui::BeginChild("Battle slots",ImVec2(layout.leftWidth,height),0,ImGuiWindowFlags_NoNavInputs);
  const float rowHeight=(std::max)(100*s,(ImGui::GetContentRegionAvail().y-3*8*s)/4);
  for(auto i:parts.tables)tableCard(rows[i],rowHeight);
  ImGui::EndChild();ImGui::SameLine(0,gap);
  ImGui::BeginChild("Room community",ImVec2(0,layout.communityHeight),0,ImGuiWindowFlags_NoNavInputs);
  ImGui::TextUnformatted(loc::Tf("room.members_heading",v.room.members.size(),v.room.capacity).c_str());
  // Whole cards only. A free fraction of the board height always clipped the
  // bottom card through its portrait.
  ImGui::BeginChild("Member list",ImVec2(0,layout.memberHeight),0,ImGuiWindowFlags_NoNavInputs|ImGuiWindowFlags_AlwaysUseWindowPadding);
  for(auto i:parts.members)memberCard(rows[i]);
  ImGui::EndChild();
  DrawRoomChat(v,action,ImGui::GetContentRegionAvail().y);
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(origin.x+layout.leftWidth+gap,origin.y+layout.communityHeight+gap));
  ImGui::BeginChild("Room actions",ImVec2(layout.rightWidth,layout.toolsHeight),0,ImGuiWindowFlags_NoNavInputs);
  const float w=(ImGui::GetContentRegionAvail().x-(layout.columns-1)*8*s)/layout.columns;
  for(std::size_t i=0;i<parts.actions.size();++i){if(i%layout.columns)ImGui::SameLine(0,8*s);button(rows[parts.actions[i]],w);}
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(origin.x+layout.leftWidth+gap,origin.y+layout.communityHeight+layout.toolsHeight+2*gap));
  ImGui::BeginChild("Room explanation",ImVec2(layout.rightWidth,layout.explanationHeight),0,ImGuiWindowFlags_NoNavInputs|ImGuiWindowFlags_NoScrollbar);
  const auto e=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& row){return row.id==nav.Focus();});
  if(e!=rows.end()){
   std::string explanation=choiceDetail(*e).empty()?e->detail:choiceDetail(*e);
   const std::string reason=hint(*e);
   if(!reason.empty())explanation=reason+(explanation.empty()?"":": "+explanation);
   NoteDetailText(explanation,e->detailText);
   ImGui::PushStyleColor(ImGuiCol_Text,shown(*e)?ToneColor(Tone::Neutral):ToneColor(Tone::Pending));
   const auto available=ImGui::GetContentRegionAvail();
   const bool clipped=ImGui::CalcTextSize(explanation.c_str(),nullptr,false,available.x).y>available.y;
   ImGui::TextWrapped("%s",explanation.c_str());ImGui::PopStyleColor();
   if(clipped&&ImGui::IsWindowHovered()){ImGui::BeginTooltip();ImGui::PushTextWrapPos(480*s);ImGui::TextUnformatted(explanation.c_str());ImGui::PopTextWrapPos();ImGui::EndTooltip();}
  }
  ImGui::EndChild();
 }else{
  ImGui::BeginChild("Room stacked",ImVec2(0,layout.stackHeight),0,ImGuiWindowFlags_NoNavInputs);
  for(auto i:parts.tables){const auto& e=rows[i];tableCard(e,(DescribeTableBanner(v,v.room.tables[std::stoi(e.id.substr(6))]).text.empty()?136:136+TableBannerHeight)*s);}
  for(auto i:parts.members)memberCard(rows[i]);
  for(auto i:parts.actions)button(rows[i],ImGui::GetContentRegionAvail().x);
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+layout.stackHeight+gap));
  ImGui::BeginChild("Room inline chat",ImVec2(0,layout.chatHeight),0,ImGuiWindowFlags_NoNavInputs);
  DrawRoomChat(v,action,ImGui::GetContentRegionAvail().y);
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+height-layout.stackedExplanation+4*s));
  ImGui::BeginChild("Room action explanation",ImVec2(0,layout.stackedExplanation-4*s));
  const auto selected=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==nav.Focus();});
  if(selected!=rows.end()){
   std::string explanation=selected->detail;
   if(!choiceDetail(*selected).empty())explanation=choiceDetail(*selected);
   else if(selected->id.compare(0,6,"table-")==0){const auto& table=v.room.tables[selectedTable_];const auto* local=Member(v.room,v.room.localMember);
    explanation=loc::Tf("room.table_explanation",PhaseName(table),local?StatusName(local->status):loc::T("room.connecting"),table.queue.size(),table.spectators.size()+table.watchingNext.size());
    // A narrow card can cut its banner short; here it is in full, and on
    // your own seat during a held start, with what Cancel the start does.
    const auto banner=DescribeTableBanner(v,table);
    const std::string first=table.spectatorHold&&place.unready&&place.table==selectedTable_?selected->detail:banner.text;
    if(!first.empty())explanation=first+"\n"+explanation;}
   const std::string reason=hint(*selected);
   NoteDetailText(selected->detail,selected->detailText);
   if(!reason.empty())explanation=explanation.empty()?reason:explanation+"\n"+reason;
   ImGui::PushStyleColor(ImGuiCol_Text,reason.empty()?ToneColor(Tone::Neutral):ToneColor(Tone::Pending));
   const auto available=ImGui::GetContentRegionAvail();
   const bool clipped=ImGui::CalcTextSize(explanation.c_str(),nullptr,false,available.x).y>available.y;
   ImGui::TextWrapped("%s",explanation.c_str());ImGui::PopStyleColor();
   if(clipped&&ImGui::IsWindowHovered()){ImGui::BeginTooltip();ImGui::PushTextWrapPos(480*s);ImGui::TextUnformatted(explanation.c_str());ImGui::PopTextWrapPos();ImGui::EndTooltip();}
  }
  ImGui::EndChild();
 }
 ImGui::PopStyleColor();ImGui::PopStyleVar();roomBoardFocus_=nav.Focus();
}
const char* ApplicationShell::PlaceExitLabel(const ShellView& v) const {
 const auto place=ExitFromPlace(v);
 return OnOwnPlace(place,menu_.navigation.Focus())?place.label:"";
}
void ApplicationShell::OpenTableOptions(const ShellView& v,int table) {
 auto& nav=menu_.navigation;
 selectedTable_=table;rulesDirty_=false;rulesRevision_=v.room.tables[table].revision;tableRules_=v.room.tables[table].rules;
 nav.PushOrUnwind("room-table");
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
 else if(!control.refusal.empty())
  Refuse(control.refusal,[table=selectedTable_,seat=local?local->seat:-1](const ShellView& now){
   return DescribeReady(now,now.room.tables[table],seat).kind==ReadyControl::None;});
}
void ApplicationShell::LeavePlace(const ShellView& v,const Submit& submit) {
 const auto exit=ExitFromPlace(v);if(exit.table<0)return;
 selectedTable_=exit.table;
 if(!exit.allowed){Refuse(exit.blocker,LeaveBlocked);return;}
 room::Action request;request.table=static_cast<std::uint8_t>(selectedTable_);request.kind=room::ActionKind::Unqueue;
 if(exit.unready){request.kind=room::ActionKind::Unready;request.seat=exit.seatIndex;}
 if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
}
bool ApplicationShell::LeaveWatchedGame(const ShellView& v,const Submit& submit) {
 const auto leave=netplay::LeaveOfWatchedGame(v.room,v.watchedGame);if(leave==netplay::WatchedLeave::None)return false;
 const auto table=static_cast<std::size_t>(v.watchedGame.table);selectedTable_=v.watchedGame.table;
 // A leave the room still takes waits for it as any press does; the rest of a
 // finished game's stream asks nothing of the room.
 if(leave!=netplay::WatchedLeave::EndView&&!RoomTakesNow(v,room::ActionKind::Unwatch,table)){
  Refuse(RoomWaitReason(v),[table](const ShellView& now){return LeavesWatchedGame(now,table)&&!RoomTakesNow(now,room::ActionKind::Unwatch,table);});
  return false;
 }
 ShellAction request;request.command.generation=v.session.generation;request.leaveWatchedGame=v.watchedGame;
 if(!submit(std::move(request))){error_=loc::T("error.queue_failed");return false;}
 error_.clear();
 return true;
}
void ApplicationShell::TrackLiveGames(const ShellView& v,double now) {
 for(std::size_t i=0;i<liveGames_.size();++i){
  auto& game=liveGames_[i];const auto& table=v.room.tables[i];
  if(table.phase!=room::TablePhase::Playing)game.since=-1;
  else if(game.since<0||game.since>now||game.generation!=table.matchGeneration){game.generation=table.matchGeneration;game.since=now;}
 }
}
// Leaving the table the lock-in held says why whoever caused it. A lock-in that
// ends while still watching that table is the player's own Release, or the
// release a failed view owes, which the runtime counts instead.
void ApplicationShell::TrackLockIn(const ShellView& v,double now) {
 const auto* local=Member(v.room,v.room.localMember);
 int lockedAt=-1;
 if(local&&local->spectatorLocked)
  for(const auto& t:v.room.tables)if(room::WatchesByChoice(t,local->id)){lockedAt=t.id;break;}
 const char* reason=nullptr;
 if(lockedInTable_>=0&&lockedAt<0&&local&&v.room.roomEpoch==lockInEpoch_&&!room::WatchesByChoice(v.room.tables[lockedInTable_],local->id)){
  bool elsewhere=false;
  for(const auto& t:v.room.tables)elsewhere=elsewhere||room::WatchesByChoice(t,local->id);
  reason=room::PlaceOf(v.room,local->id).kind!=room::Place::Kind::None?"room.lock_in_ended.playing":
   elsewhere?"room.lock_in_ended.moved":"room.lock_in_ended.stopped";
 }
 if(v.lockInDrops>lockInDropsSeen_)reason="room.lock_in_ended.dropped";
 if(reason){notice_=loc::T(reason);noticeTone_=Tone::Pending;noticeUntil_=now+8;}
 lockedInTable_=lockedAt;lockInEpoch_=v.room.roomEpoch;lockInDropsSeen_=v.lockInDrops;
}
bool ApplicationShell::GameIsStale(std::size_t table) const {
 return table<liveGames_.size()&&liveGames_[table].since>=0&&ImGui::GetTime()-liveGames_[table].since>=room::StaleGameSeconds;
}
// X, Y and View open the fighter, table options and chat; the same button
// again returns to the page that opened it.
void ApplicationShell::RoomShortcut(const MenuAction& a,const ShellView& v) {
 auto& nav=menu_.navigation;
 // Delete means something only on your own card (DrawRoomBoard).
 if(a.delta==MenuInput::Leave)return;
 const char* target=a.delta==MenuInput::Fighter?"selection":a.delta==MenuInput::Options?"room-table":"room-chat";
 if(nav.Screen()==target){nav.Return();return;}
 if(a.delta==MenuInput::Fighter){
  const auto blocker=SelectionBlocker(v);
  if(!blocker.empty()){
   Refuse(blocker,[](const ShellView& now){return !SelectionBlocker(now).empty();});
   return;
  }
 }else if(a.delta==MenuInput::Options){
  // The focused table on the board, otherwise the member's own.
  const auto* local=Member(v.room,v.room.localMember);
  const bool onTable=nav.Screen()=="room"&&nav.Focus().compare(0,6,"table-")==0;
  OpenTableOptions(v,onTable?selectedTable_:OptionsTable(v,selectedTable_));
  return;
 }
 if(a.delta==MenuInput::Fighter){selectionFresh_=true;selectionOpenOn_="roster";}
 nav.PushOrUnwind(target);
}
void ApplicationShell::RoomAction(const MenuAction& a,const ShellView& v,const Submit& submit) {
 using namespace room;auto& nav=menu_.navigation;
 const auto sendDelay=[&](netplay::CommandKind kind,int selected){
  if(!RoomActionsAvailable(v) || v.delayLocked ||
     (kind==netplay::CommandKind::CheckConnection && (!v.canProbe || v.probeStatus=="checking"))) {
   Refuse(RoomWaitReason(v),[](const ShellView& now){return !RoomActionsAvailable(now)||now.delayLocked;});return false;
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
  if(a.text=="leave-seat"){LeavePlace(v,submit);return;}
  if(a.text=="ready"||a.text=="unready"){ToggleReady(v,submit);return;}
  if(a.text=="selection"){RoomShortcut({MenuAction::Shortcut,{},{},MenuInput::Fighter},v);return;}
  if(a.text=="leave-place"||a.text=="cancel-start"){RoomAction({MenuAction::Activate,"leave-place"},v,submit);return;}
  const auto* option=FindSeatOption(a.text);
  if(!option)return;
  Action request;request.table=static_cast<std::uint8_t>(selectedTable_);request.kind=option->kind;request.seat=static_cast<std::int8_t>(option->seat);
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
  return;
 }
 if(a.id.compare(0,6,"table-")==0){
  selectedTable_=std::stoi(a.id.substr(6));
  OpenTableOptions(v,selectedTable_);
  return;
 }
 if(a.id=="room-match"||a.id=="room-rules"){
  selectedTable_=OptionsTable(v,selectedTable_);
  rulesDirty_=false;rulesRevision_=v.room.tables[selectedTable_].revision;tableRules_=v.room.tables[selectedTable_].rules;
  nav.Push(a.id);return;
 }
 if(a.id=="rules-table"){
  if(rulesDirty_||v.room.tournament.Active())return;
  selectedTable_=(std::max)(0,(std::min)(static_cast<int>(v.room.tables.size())-1,selectedTable_+a.delta));
  rulesRevision_=v.room.tables[selectedTable_].revision;tableRules_=v.room.tables[selectedTable_].rules;return;
 }
 if(a.id=="options"){OpenTableOptions(v,OptionsTable(v,selectedTable_));return;}
 // End practice is about the member's own table, wherever the board's focus is.
 if(a.id=="end-practice")selectedTable_=OptionsTable(v,selectedTable_);
 if(a.id=="stop-watching"){LeaveWatchedGame(v,submit);return;}
 if(a.id=="leave-place"){
  const auto exit=ExitFromPlace(v);if(exit.table<0)return;
  selectedTable_=exit.table;
  // A seat that would lose a score or pass to the queue is asked about first;
  // the board opens the question on the next frame, with the answer in Chosen.
  if(exit.seat&&exit.allowed&&exit.cost){leaveAsk_=exit.table;leaveAsked_=false;return;}
  LeavePlace(v,submit);
  return;
 }
 if(a.id.compare(0,7,"member-")==0){selectedMember_=std::stoull(a.id.substr(7));nav.Push("room-member");return;}
 if(a.id=="room-members"||a.id=="room-chat"||a.id=="room-admin"||a.id=="room-invite"){nav.Push(a.id);return;}
  if(a.id=="copy"){ImGui::SetClipboardText(v.invitation.c_str());error_.clear();shortCopyPending_=false;notice_=loc::T("room.invitation_copied");noticeTone_=Tone::Success;noticeUntil_=ImGui::GetTime()+3;return;}
  if(a.id=="copy-short"){CopyShortInvitation(v,submit);return;}
  if(a.id=="copy-room-link"){
   const auto link=publicRooms_.RoomLink();
   if(!link.empty()){ImGui::SetClipboardText(link.c_str());error_.clear();notice_=loc::T("public.link_copied");noticeTone_=Tone::Success;noticeUntil_=ImGui::GetTime()+3;}
   return;
  }
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
  if(a.id=="input-delay"){
   if(netplay::CanSaveUnseatedInputDelay(v.session,v.room)){
    if(a.kind!=MenuAction::Adjust)return;
    ShellAction request;request.command.kind=netplay::CommandKind::SaveInputDelay;request.command.generation=v.session.generation;
    request.selectedDelay=StepInputDelay(v.preferences.autoInputDelay,v.preferences.inputDelay,a.delta);
    if(!submit(std::move(request)))error_=loc::T("error.queue_failed");else error_.clear();
    return;
   }
   // Select takes the recommendation; Left and Right choose a delay, or Auto before the smallest.
   const int selected=a.kind==MenuAction::Adjust?StepInputDelay(v.preferences.autoInputDelay,v.selectedDelay,a.delta):-1;
   sendDelay(netplay::CommandKind::ApplyDelay,selected);return;
  }
  if(a.id=="ultra"||a.id=="appearance"){
   // Select shows the Ultra or costume cards (a costume goes on to its
   // colors), and picking one comes back here.
   const bool ultra=a.id=="ultra";
   if(a.kind==MenuAction::Adjust){
    if(!v.canEditSelection||!RoomActionsAvailable(v))return;
    ShellAction step;step.command.generation=v.session.generation;
    step.selectionStep={ultra?ShellAction::SelectionStep::Field::Ultra:ShellAction::SelectionStep::Field::Color,a.delta};submit(std::move(step));
   }else if(SelectionBlocker(v).empty()){selectionFresh_=true;selectionOpenOn_=ultra?"ultra":"costumes";nav.Push("selection");}
   return;
  }
  if(a.id=="stage"||a.id=="fighter-options"){
   if(a.kind==MenuAction::Activate&&SelectionBlocker(v).empty()){selectionFresh_=true;selectionOpenOn_=a.id=="stage"?"stage":"options";nav.Push("selection");}
   return;
  }
 if(a.id=="leave"){Send(netplay::CommandKind::LeaveRoom,v,submit);return;}
 if(a.id=="room-training"){
  // No room command: the runtime sends the game on, if the room still allows it, while the room stays as it is.
  ShellAction training;training.command.generation=v.session.generation;training.training=TrainingEntry::Room;
  if(!v.canTrain||!submit(std::move(training)))error_=loc::T("room.training.unavailable");
  return;
 }
 if(a.id=="steam-profile"){
  // The runtime opens Steam's overlay on the profile when it is up, and the browser page otherwise.
  const auto* m=Member(v.room,selectedMember_);
  if(!m||!m->steamId.Present()){error_=loc::T("room.member_left.detail");return;}
  ShellAction open;open.command.generation=v.session.generation;
  open.service=platform::ServiceAction::OpenSteamProfile;open.steamProfile=m->steamId;
  if(!submit(std::move(open)))error_=loc::T("error.queue_failed");else error_.clear();
  return;
 }
 if(a.id=="mute"){if(muted_.count(selectedMember_))muted_.erase(selectedMember_);else muted_.insert(selectedMember_);return;}
 if(a.id=="rename"){std::snprintf(roomName_,sizeof(roomName_),"%s",a.text.c_str());return;}
 if(a.id=="room-capacity"){roomCapacity_=(std::max)(2,(std::min)(16,roomCapacity_+a.delta));return;}
 if(AdjustRule(tableRules_,a)){rulesDirty_=!(tableRules_==v.room.tables[selectedTable_].rules);return;}
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
 else if(a.id=="abandon-result"||a.id=="end-practice")request.kind=ActionKind::AbortMatch;
 else if(a.id=="apply-rules"){
  // Applying clears Ready, so rules equal to the table's are never sent.
  if(tableRules_==v.room.tables[selectedTable_].rules){rulesDirty_=false;return;}
  request.kind=ActionKind::SetRules;request.rules=tableRules_;
 }
 else if(a.id=="apply-name"){request.kind=ActionKind::Rename;request.text=roomName_;}
 else if(a.id=="apply-capacity"){request.kind=ActionKind::SetCapacity;request.capacity=static_cast<std::uint8_t>(roomCapacity_);}
 else if(a.id=="lock"){request.kind=ActionKind::Lock;request.locked=a.delta<0;}
 else if(a.id=="compose"){
  // The draft stays in the box until the room's chat has it (ObserveChat); the same text is not sent twice while it is on its way.
  // A room that cannot take it yet holds it, and FlushChat sends it then; one held is the only one accepted.
  if(!HasChatText(chat_)||queuedChat_||(ChatInFlight(ImGui::GetTime())&&pendingChat_->text==chat_))return;
  queuedChat_=QueuedChat{chat_};FlushChat(v,submit);return;
 }
 else if(a.id=="ready"){ToggleReady(v,submit);return;}
 else return;
 if(SendRoom(std::move(request),v,submit)){
  if(a.id=="apply-rules")rulesDirty_=false;
 }else if(error_.empty())error_=loc::T("room.action_rejected");
}
} }
