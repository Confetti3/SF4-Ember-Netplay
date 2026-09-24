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
    if (!submit(std::move(request))) { error_ = loc::T("room.action_queue_failed"); return false; }
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
 if(tableGeneration_!=t.matchGeneration){tableGeneration_=t.matchGeneration;nav.Cancel();}
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
   }
   for(const auto& m:s.members)rows.push_back(Row("member-"+std::to_string(m.id),loc::Tf(m.id==s.localMember?"room.member_you":"room.member",m.name),loc::Tf(m.host?"room.member_status_host":"room.member_status",StatusName(m.status))));
   rows.push_back(Row("room-members",loc::T("room.members"),loc::Tf("room.member_count",s.members.size(),s.capacity)));
   rows.push_back(Row("room-chat",loc::T("room.chat"),loc::T("room.chat.detail")));
   rows.push_back(Row("selection",loc::T("room.change_fighter"),mutableRoom&&v.canEditSelection&&!s.localTerminalPending?v.selectionSummary:
    (s.localTerminalPending?TerminalPendingReason():loc::T("room.change_fighter.open_table")),
    mutableRoom&&v.canEditSelection&&!s.localTerminalPending));
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
   const bool ready=t.phase==TablePhase::Waiting&&t.ready[local->seat];
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
    const bool canUnready=mutableRoom&&ready&&t.phase==TablePhase::Waiting;
    // Ready is the player's one job here. Everything the room is still
    // finishing (draining, checkpoint, receipt, result) stays behind the
    // runtime, which parks the press and reports only a real failure.
    const bool roomReachable=mutableRoom||RoomCheckpointPending(v);
    const bool finishedGame=t.phase==TablePhase::Playing&&v.session.match==netplay::MatchState::PostMatch;
    const bool readyable=roomReachable&&!ready&&!v.readyRequested&&(t.phase==TablePhase::Waiting||finishedGame)&&
     t.p1&&t.p2&&v.controllerReady&&v.selectionError.empty();
    const std::string readyDetail=ready?loc::T("room.ready.cancel_detail"):
     v.readyRequested?loc::T("room.ready.locking"):
     readyable?std::string(loc::T("room.ready.lock_detail"))+"\n"+v.selectionSummary:
     !roomReachable?reason:
     t.phase==TablePhase::Paused?loc::T("room.result_unresolved.detail"):
     t.phase==TablePhase::Ready?loc::T("room.both_ready"):
     t.phase==TablePhase::Playing?loc::T("room.match_in_progress.detail"):
     !t.p1||!t.p2?loc::T("room.waiting_other_seat"):
     !v.controllerReady?loc::T("room.controller_required"):v.selectionError;
   rows.push_back(Row("ready",loc::T(ready?"room.unready":v.readyRequested?"room.readying":
    t.phase==TablePhase::Paused?"room.result_unresolved":t.phase==TablePhase::Ready?"room.preparing_match":
    t.phase==TablePhase::Playing&&!finishedGame?"room.match_in_progress":
    v.session.match==netplay::MatchState::PostMatch?"room.ready_rematch":"room.ready_up"),readyDetail,canUnready||readyable));
    const bool delayEditable=mutableRoom&&!active&&!v.delayLocked;
    const int selectedDelay=(std::max)(0,(std::min)(10,v.selectedDelay));
    const bool recommended=v.recommendedDelay>=0&&v.recommendedDelay<=10;
    const auto check=DescribeConnectionCheck(v);
    auto recommendedRow=Value("recommended-delay",loc::T("room.recommended_delay"),check.value,check.detail,false);
    recommendedRow.adjustable=false;rows.push_back(std::move(recommendedRow));
    rows.push_back(Value("selected-delay",loc::T("room.selected_delay"),std::to_string(selectedDelay),
     delayEditable?loc::T("room.selected_delay.detail"):
      (v.delayLocked?loc::T("room.selected_delay.locked"):reason),delayEditable));
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
  for(const auto& m:s.members)rows.push_back(Row("member-"+std::to_string(m.id),loc::Tf(m.id==s.localMember?"room.member_you":"room.member",m.name),
   loc::Tf(m.host?(muted_.count(m.id)?"room.member_status_host_muted":"room.member_status_host"):(muted_.count(m.id)?"room.member_status_muted":"room.member_status"),StatusName(m.status))));
  }else if(screen=="room-member"){
   const auto* m=Member(s,selectedMember_);const bool other=m&&m->id!=s.localMember;
   rows.push_back(Row("mute",loc::T(muted_.count(selectedMember_)?"room.unmute_member":"room.mute_member"),loc::T(m?"room.mute_member.detail":"room.member_left.detail"),other));
   rows.push_back(ConfirmRow("kick",loc::T("room.kick_member"),m?loc::Tf("room.kick_member.detail",m->name):loc::T("room.member_left.detail"),host&&other));
   rows.push_back(ConfirmRow("transfer-host",loc::T("room.transfer_host"),m?loc::Tf("room.transfer_host.detail",m->name):loc::T("room.member_left.detail"),host&&other));
 }else if(screen=="room-chat"){
   rows.push_back(TextRow("compose",loc::T("room.compose_message"),chat_,MaximumChatBytes,mutableRoom));
   rows.push_back(Row("send-chat",loc::T("room.send_message"),loc::T("room.send_message.detail"),mutableRoom&&chat_[0]));
  for(auto it=s.chat.rbegin();it!=s.chat.rend();++it)if(!muted_.count(it->sender))
   rows.push_back(Row("message-"+std::to_string(it->sequence),Name(s,it->sender),it->text));
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
  const bool clicked=ImGui::Button("##room-entry",size);
  ImGui::PopStyleColor();ImGui::PopID();
  if(clicked&&!nav.Confirming()&&!nav.Editing()){nav.Focus(e.id,rows);action=nav.Choose(rows);}
  if(e.id==focus){
   ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),palette::Ember,0,0,2*s);
   if(changed)ImGui::SetScrollHereY(.5f);
  }
 };
 const auto fighter=[&](const room::Member* m){return m?(m->id==v.room.localMember?v.selectedFighter:m->fighter):-1;};
 const auto tableCard=[&](const MenuEntry& e,float h){
  const auto& t=v.room.tables[std::stoi(e.id.substr(6))];const auto p=ImGui::GetCursorScreenPos();
  const float width=ImGui::GetContentRegionAvail().x;press(e,ImVec2(width,h));
  text(ImVec2(p.x+12*s,p.y+8*s),width*.57f,loc::Tf("room.battle_slot",t.id+1),14*s,palette::Ivory);
  text(ImVec2(p.x+width*.60f,p.y+8*s),width*.40f-12*s,PhaseName(t.phase),12*s,palette::Ember);
  // A full pair shows its running win count where "VS" would sit; the gap
  // between the two sides grows to fit however long the rematch run gets.
  const std::string middle=t.p1&&t.p2?SetScoreText(t.score):"VS";
  const float gap=(std::max)(34*s,ImGui::GetFont()->CalcTextSizeA(16*s,FLT_MAX,0,middle.c_str()).x+12*s);
  // Portraits sit on the card's outer edges, mirrored, so neither crowds the
  // score. The row fills and centres in the band between header and footer.
  const float band=h-54*s,portrait=(std::min)(96*s,band),half=(width-24*s-gap)*.5f;
  const float top=p.y+28*s+(band-portrait)*.5f+portrait*.5f-20*s;
  const room::MemberId ids[]={t.p1,t.p2};
  for(int side=0;side<2;++side){
   const float x=p.x+12*s+side*(half+gap);const auto* m=Member(v.room,ids[side]);const int id=fighter(m);
   const float px=side?x+half-portrait:x,py=p.y+28*s+(band-portrait)*.5f;
   if(m)DrawCharacterPortrait(id,ImVec2(px,py),ImVec2(px+portrait,py+portrait));
   const float tx=x+(m&&!side?portrait+8*s:0),tw=half-(m?portrait+8*s:0);
   // Names of different lengths read ragged when flush left; centre each
   // in its own slot so the pair stays symmetric about VS.
   text(ImVec2(tx,top+2*s),tw,m?m->name:loc::T("room.looking_for_fight"),16*s,m?palette::Ivory:palette::Muted,true);
   const auto* f=selection::FindFighter(id);
   // An empty seat already reads "Looking for a fight" above; a second
   // "Open seat" underneath said nothing new.
   std::string caption=m?(f?f->name:loc::T("room.fighter_not_shared")):std::string();
   const bool ready=t.phase==room::TablePhase::Waiting&&t.ready[side];
   if(m)caption+=ready?loc::T("room.suffix_ready"):m->id==v.room.localMember?loc::T("room.suffix_you"):"";
   if(!caption.empty())text(ImVec2(tx,top+21*s),tw,caption,12*s,ready?palette::Ready:palette::Muted,true);
  }
  text(ImVec2(p.x+12*s+half,top+18*s),gap,middle,16*s,palette::Ember,true);
  text(ImVec2(p.x+12*s,p.y+h-21*s),width-24*s,loc::Tf("room.table_footer",t.rules.roundCount,t.rules.roundTime,t.queue.size(),t.spectators.size()+t.watchingNext.size()),12*s,palette::Muted);
  tip();
 };
 const auto memberCard=[&](const MenuEntry& e){
  const auto* m=Member(v.room,std::stoull(e.id.substr(7)));if(!m)return;
  const auto p=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
  const int main=m->id==v.room.localMember?v.preferences.mainFighter:m->mainFighter;
  press(e,ImVec2(width,58*s));DrawCharacterPortrait(main,ImVec2(p.x+6*s,p.y+5*s),ImVec2(p.x+54*s,p.y+53*s));
  text(ImVec2(p.x+64*s,p.y+7*s),width-76*s,e.label,16*s,palette::Ivory);
  const auto status=loc::Tf(m->table>=0?(m->host?"room.board_status_host_slot":"room.board_status_slot"):(m->host?"room.board_status_host":"room.board_status"),StatusName(m->status),m->table+1);
  text(ImVec2(p.x+64*s,p.y+32*s),width-76*s,status,12*s,palette::Muted);
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
  const auto origin=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
  const int toolColumns=(std::max)(3,(std::min)(6,static_cast<int>(width/(160*s))));
  const float toolsHeight=std::ceil(toolbar.size()/float(toolColumns))*48*s+28*s;
  const float boardHeight=(std::max)(180*s,height-toolsHeight-gap),leftWidth=(width-gap)*.50f;
  ImGui::BeginChild("Battle slots",ImVec2(leftWidth,boardHeight),0,ImGuiWindowFlags_NoNavInputs);
  const float rowHeight=(std::max)(100*s,(ImGui::GetContentRegionAvail().y-3*8*s)/4);
  for(const auto& e:rows)if(e.id.compare(0,6,"table-")==0)tableCard(e,rowHeight);
  ImGui::EndChild();ImGui::SameLine(0,gap);
  ImGui::BeginChild("Room community",ImVec2(0,boardHeight),0,ImGuiWindowFlags_NoNavInputs);
  ImGui::TextUnformatted(loc::Tf("room.members_heading",v.room.members.size(),v.room.capacity).c_str());
  // Whole cards only. A free fraction of the board height always clipped the
  // bottom card through its portrait.
  const float cardPitch=58*s+ImGui::GetStyle().ItemSpacing.y,childPadding=12*s;
  const int memberRows=(std::max)(1,static_cast<int>(((boardHeight-48*s)*.56f-childPadding+ImGui::GetStyle().ItemSpacing.y)/cardPitch));
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
  ImGui::SetCursorScreenPos(ImVec2(origin.x,origin.y+boardHeight+gap));
  ImGui::BeginChild("Room actions",ImVec2(width,toolsHeight),0,ImGuiWindowFlags_NoNavInputs);
  const float w=(ImGui::GetContentRegionAvail().x-(toolColumns-1)*8*s)/toolColumns;
  for(std::size_t i=0;i<toolbar.size();++i){if(i%toolColumns)ImGui::SameLine(0,8*s);button(*toolbar[i],w);}
  const auto e=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& row){return row.id==nav.Focus();});
  if(e!=rows.end()){
   // The wide board has one line for the focused row's explanation. Lines
   // after the first (the fighter summary under Ready, for example) and any
   // elided text are reachable on hover, like the narrow board's cards.
   const auto newline=e->detail.find('\n');
   std::string line=e->detail.substr(0,newline);
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
   if(selected->id.compare(0,6,"table-")==0){const auto& table=v.room.tables[selectedTable_];const auto* local=Member(v.room,v.room.localMember);
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
void ApplicationShell::RoomAction(const MenuAction& a,const ShellView& v,const Submit& submit) {
 using namespace room;auto& nav=menu_.navigation;
 const auto sendDelay=[&](netplay::CommandKind kind,int selected){
  if(!RoomActionsAvailable(v) || v.delayLocked ||
     (kind==netplay::CommandKind::CheckConnection && (!v.canProbe || v.probeStatus=="checking"))) {
   error_=RoomWaitReason(v);return false;
  }
  ShellAction request;request.command.kind=kind;request.command.generation=v.session.generation;request.selectedDelay=selected;
  if(!submit(std::move(request))){error_=loc::T("room.action_queue_failed");return false;}
  error_.clear();return true;
 };
 if(a.id.compare(0,6,"table-")==0){
  selectedTable_=std::stoi(a.id.substr(6));rulesDirty_=false;rulesRevision_=v.room.tables[selectedTable_].revision;
  tableRules_=v.room.tables[selectedTable_].rules;nav.Push("room-table");return;
 }
 if(a.id.compare(0,7,"member-")==0){selectedMember_=std::stoull(a.id.substr(7));nav.Push("room-member");return;}
 if(a.id=="room-members"||a.id=="room-chat"||a.id=="room-admin"||a.id=="room-rules"){nav.Push(a.id);return;}
  if(a.id=="copy"){ImGui::SetClipboardText(v.invitation.c_str());error_.clear();notice_=loc::T("room.invitation_copied");noticeTone_=Tone::Success;noticeUntil_=ImGui::GetTime()+3;return;}
  if(a.id=="replace-room"){ShellAction request;request.command.kind=netplay::CommandKind::ReplaceRoom;request.command.generation=v.session.generation;
   if(!submit(std::move(request)))error_=loc::T("room.action_queue_failed");
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
  else if(a.id=="kick"){request.kind=ActionKind::Kick;request.target=selectedMember_;}
  else if(a.id=="transfer-host"){request.kind=ActionKind::TransferHost;request.target=selectedMember_;}
 else if(a.id=="cancel-result")request.kind=ActionKind::CancelResult;
 else if(a.id=="abandon-result")request.kind=ActionKind::AbortMatch;
 else if(a.id=="apply-rules"){request.kind=ActionKind::SetRules;request.rules=tableRules_;}
 else if(a.id=="apply-name"){request.kind=ActionKind::Rename;request.text=roomName_;}
 else if(a.id=="apply-capacity"){request.kind=ActionKind::SetCapacity;request.capacity=static_cast<std::uint8_t>(roomCapacity_);}
 else if(a.id=="lock"){request.kind=ActionKind::Lock;request.locked=a.delta<0;}
 else if(a.id=="send-chat"){request.kind=ActionKind::Chat;request.text=chat_;}
 else if(a.id=="ready"){
  const auto* local=Member(v.room,v.room.localMember);const auto& table=v.room.tables[selectedTable_];
  if(local&&local->seat>=0&&local->seat<2&&table.ready[local->seat]&&table.phase==TablePhase::Waiting){
   if(!RoomActionsAvailable(v)) { error_=RoomWaitReason(v);return; }
   request.kind=ActionKind::Unready;request.seat=local->seat;
  }else{
   // Ready is never refused here for a transient room state; the runtime
   // parks it and reports a failure through the notice.
   Send(v.session.match==netplay::MatchState::PostMatch?netplay::CommandKind::Rematch:netplay::CommandKind::Ready,v,submit);return;
  }
 }else return;
 if(SendRoom(std::move(request),v,submit)){
  if(a.id=="send-chat")chat_[0]=0;
  if(a.id=="apply-rules")rulesDirty_=false;
 }else if(error_.empty())error_=loc::T("room.action_rejected");
}
} }
