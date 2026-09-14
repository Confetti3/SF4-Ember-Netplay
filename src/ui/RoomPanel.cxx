#include "ApplicationShell.hxx"
#include "Theme.hxx"
#include "MenuRows.hxx"
#include "RoomFeedback.hxx"
#include "../common/FighterCatalog.hxx"
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
const char* Name(const room::Snapshot& snapshot, room::MemberId id) {
    const auto* member = Member(snapshot, id);
    return member ? member->name.c_str() : (id ? "Member left" : "Open seat");
}
const char* StatusName(room::MemberStatus status) {
    switch (status) {
    case room::MemberStatus::Queued: return "Queued";
    case room::MemberStatus::Seated: return "Seated";
    case room::MemberStatus::Ready: return "Ready";
    case room::MemberStatus::Playing: return "Playing";
    case room::MemberStatus::Watching: return "Watching";
    case room::MemberStatus::WatchingNext: return "Watching next game";
    default: return "Idle";
    }
}
const char* PhaseName(room::TablePhase phase) {
    switch (phase) {
    case room::TablePhase::Waiting: return "Waiting for fighters";
    case room::TablePhase::Ready: return "Preparing game";
    case room::TablePhase::Playing: return "In game";
    case room::TablePhase::Paused: return "Result unresolved";
    case room::TablePhase::Closed: return "Closed";
    default: return "Open";
    }
}
const char* TerminalPendingReason() {
    return "Waiting for all players and spectators to finish returning from the previous match.";
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
    const bool terminalBlocked = view.room.localTerminalPending || view.room.terminalPending[action.table];
    if (terminalBlocked && (action.kind == room::ActionKind::Queue || action.kind == room::ActionKind::Watch ||
        action.kind == room::ActionKind::Ready || action.kind == room::ActionKind::Unready)) {
        error_ = TerminalPendingReason();
        return false;
    }
    const bool localSeated = local && local->table == static_cast<std::int8_t>(action.table) &&
        local->seat >= 0 && local->seat < 2;
    const bool tableActive = table.phase == room::TablePhase::Ready || table.phase == room::TablePhase::Playing ||
        table.phase == room::TablePhase::Paused;
    if ((action.kind == room::ActionKind::Queue || action.kind == room::ActionKind::Watch) && localSeated) return false;
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
    if (!submit(std::move(request))) { error_ = "The room action could not be queued. Please try again."; return false; }
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
    std::string detail=std::string(Name(s,table.p1))+" vs "+Name(s,table.p2)+"\n"+PhaseName(table.phase)+
     "\nQueue: "+std::to_string(table.queue.size())+" / Watching: "+std::to_string(table.spectators.size()+table.watchingNext.size())+
     "\nYou: "+(local?StatusName(local->status):"Connecting");
    rows.push_back(Row("table-"+std::to_string(table.id),"Table "+std::to_string(table.id+1)+"   "+std::to_string(occupied)+"/2",detail));
   }
   for(const auto& m:s.members)rows.push_back(Row("member-"+std::to_string(m.id),m.name+(m.id==s.localMember?" / YOU":""),std::string(StatusName(m.status))+(m.host?" / HOST":"")));
   rows.push_back(Row("room-members","Members",std::to_string(s.members.size())+" / "+std::to_string(s.capacity)+" members."));
   rows.push_back(Row("room-chat","Chat","Read messages and explicitly compose text with a keyboard."));
   rows.push_back(Row("selection","Change fighter & appearance",mutableRoom&&v.canEditSelection&&!s.localTerminalPending?v.selectionSummary:
    (s.localTerminalPending?TerminalPendingReason():"Open your table for Ready status and fighter-change requirements."),
    mutableRoom&&v.canEditSelection&&!s.localTerminalPending));
   rows.push_back(Row("room-admin","Room settings",host?"Room name, capacity and admission.":"Only the host can administer the room.",host));
  }else{
   rows.push_back(Row("room-status","Connection status",v.session.room==netplay::RoomState::Opening?"Opening room...":"Waiting for room state.",false));
   if(v.canReady)rows.push_back(Row("ready","Ready","Use your saved fighter.",mutableRoom));
  }
  rows.push_back(Row("copy","Copy invitation","Share the private invitation with friends.",!v.invitation.empty()));
  rows.push_back(ConfirmRow("leave",v.session.room==netplay::RoomState::Closing?"Leaving room...":"Leave room",
   "Disconnect from this room. Hiding Ember keeps you in the room.",v.session.room!=netplay::RoomState::Closing));
  if(v.session.recovery==netplay::Recovery::ReplacementOffered)
   rows.push_back(ConfirmRow("replace-room","Replace room",v.canReplaceRoom?
    "Close the frozen room and open a replacement. Any unresolved room state will be discarded.":
    "The previous match is still closing. Replacement will be available when it finishes.",v.canReplaceRoom));
  for(std::size_t i=0;i<rows.size();++i){auto& e=rows[i];
   if(e.id.compare(0,6,"table-")==0)e.right=s.members.empty()?"room-members":"member-"+std::to_string(s.members.front().id);
   else if(e.id.compare(0,7,"member-")==0)e.left="table-"+std::to_string(selectedTable_);
   else {e.left=i?rows[i-1].id:e.id;e.right=i+1<rows.size()?rows[i+1].id:e.id;}
  }
  }else if(screen=="room-table"){
   const std::string reason=!mutableRoom?RoomWaitReason(v):
    elsewhere?"Leave your current table first.":"Choose an action for this table.";
  if(seated){
   const bool ready=t.phase==TablePhase::Waiting&&t.ready[local->seat];
   const bool terminalBlocked=s.localTerminalPending || s.terminalPending[selectedTable_];
   const bool awaitingResult=t.phase==TablePhase::Playing&&(t.resultPending||v.session.match==netplay::MatchState::PostMatch);
   const std::string blocked=!mutableRoom?reason:
    terminalBlocked?TerminalPendingReason():
    t.phase==TablePhase::Paused?"The previous result is unresolved. Ask the host to cancel the unresolved game; no win will be awarded.":
    awaitingResult?"Waiting for both fighters to report the result. Ready resets when the result is confirmed; you have not readied for another match.":
    active?"The current match is preparing or in progress. Wait for it to finish.":
    !t.p1||!t.p2?"Waiting for an opponent to take the other seat. You can change your fighter while waiting.":
    !v.controllerReady?"Assign or reconnect your controller in Settings > Player & Controller.":
    !v.selectionError.empty()?v.selectionError:!v.readyLockReason.empty()?v.readyLockReason:"Waiting for the current room update to finish.";
    const bool canUnready=mutableRoom&&ready&&t.phase==TablePhase::Waiting;
   rows.push_back(Row("ready",t.phase==TablePhase::Paused?"Result unresolved":awaitingResult?"Waiting for results":
    t.phase==TablePhase::Playing?"Match in progress":t.phase==TablePhase::Ready?"Preparing match":
    ready?"Unready / unlock fighter":v.session.match==netplay::MatchState::PostMatch?"Ready for rematch":"Ready up",
    canUnready?"You are READY. Select to cancel Ready, then choose Change fighter & appearance.":
    mutableRoom&&v.canReady&&!terminalBlocked?"Select to lock in your fighter. The match starts when both players are ready.\n"+v.selectionSummary:blocked,
     canUnready||(mutableRoom&&v.canReady&&!terminalBlocked&&t.phase==TablePhase::Waiting)));
    const bool delayEditable=mutableRoom&&!active&&!v.delayLocked;
    const int selectedDelay=(std::max)(0,(std::min)(10,v.selectedDelay));
    const bool recommended=v.recommendedDelay>=0&&v.recommendedDelay<=10;
    const auto check=DescribeConnectionCheck(v);
    auto recommendedRow=Value("recommended-delay","Recommended delay",check.value,check.detail,false);
    recommendedRow.adjustable=false;rows.push_back(std::move(recommendedRow));
    rows.push_back(Value("selected-delay","Selected delay",std::to_string(selectedDelay),
     delayEditable?"Adjust from 0 to 10 frames. Changes apply immediately.":
      (v.delayLocked?"The selected delay is locked for this match.":reason),delayEditable));
    rows.push_back(Row("check-connection",check.action,check.checking?check.detail:
     !mutableRoom?reason:!t.p1||!t.p2?"Both seats need a player before checking the connection.":
     ready?"Choose Unready to check the connection before the next match.":
     !delayEditable?"Finish the current match before checking the connection.":check.detail,
     v.canProbe&&delayEditable&&!check.checking));
    rows.push_back(Row("benchmark-connection","Benchmark connection (30 seconds)",
     "Measure gameplay-size datagrams to your opponent. Reports network performance, not game FPS.",
     v.canProbe&&delayEditable&&!check.checking));
    rows.push_back(Row("apply-recommendation","Apply recommendation",recommended?
     "Set your selected delay to "+std::to_string(v.recommendedDelay)+" frames.":"Run Check connection before applying a recommendation.",
     v.canApplyDelay&&recommended&&delayEditable&&!check.checking));
   const std::string editReason=active?blocked:ready?"Choose Unready above to unlock your fighter and appearance.":
     !mutableRoom?reason:!v.selectionLockReason.empty()?v.selectionLockReason:"Waiting for the current match or selection update to finish.";
   rows.push_back(Row("selection","Change fighter & appearance",mutableRoom&&v.canEditSelection&&!s.localTerminalPending?
    "Choose a fighter, then browse Costume and Color galleries. Return here and select Ready up.\n"+v.selectionSummary:
    (s.localTerminalPending?TerminalPendingReason():editReason),mutableRoom&&v.canEditSelection&&!s.localTerminalPending));
    rows.push_back(Row("unqueue","Leave seat",active?"Finish or resolve the current game first.":"Release your seat.",mutableRoom&&!active&&!playing));
   }else{
    const bool terminalBlocked=s.localTerminalPending || s.terminalPending[selectedTable_];
    const std::string queueReason=terminalBlocked?TerminalPendingReason():reason;
    rows.push_back(Row(queued?"unqueue":"queue",queued?"Leave queue":"Join queue",queueReason,mutableRoom&&!elsewhere&&!terminalBlocked));
    rows.push_back(Row(watching?"unwatch":"watch",watching?"Stop watching":"Watch next game",queueReason,mutableRoom&&!elsewhere&&!terminalBlocked));
  }
  rows.push_back(Row("room-rules","Table rules",host?"Edit this table's rules.":"View this table's rules."));
  if(t.phase==TablePhase::Paused)rows.push_back(ConfirmRow("cancel-result","Cancel unresolved game","Host only. No result will be recorded.",host));
  if(v.session.recovery==netplay::Recovery::ReplacementOffered)
   rows.push_back(ConfirmRow("replace-room","Replace room",v.canReplaceRoom?
    "Close the frozen room and open a replacement. Any unresolved room state will be discarded.":
    "The previous match is still closing. Replacement will be available when it finishes.",v.canReplaceRoom));
  rows.push_back(ConfirmRow("leave",v.session.room==netplay::RoomState::Closing?"Leaving room...":"Leave room",
   "Disconnect from this room and return to Home.",v.session.room!=netplay::RoomState::Closing));
 }else if(screen=="room-rules"){
  if(!rulesDirty_&&rulesRevision_!=t.revision){tableRules_=t.rules;rulesRevision_=t.revision;}
  RuleRows(rows,tableRules_,host&&!active,host?(active?"Finish the active game before changing rules.":"Changes apply only after Apply."): "Only the host can change table rules.");
  rows.push_back(Row("apply-rules","Apply rules","Submit the complete rules draft for this table.",host&&!active&&rulesDirty_));
 }else if(screen=="room-members"){
  for(const auto& m:s.members)rows.push_back(Row("member-"+std::to_string(m.id),m.name+(m.id==s.localMember?" / YOU":""),
   std::string(StatusName(m.status))+(m.host?" / HOST":"")+(muted_.count(m.id)?" / Muted locally":"")));
  }else if(screen=="room-member"){
   const auto* m=Member(s,selectedMember_);const bool other=m&&m->id!=s.localMember;
   rows.push_back(Row("mute",muted_.count(selectedMember_)?"Unmute member":"Mute member",m?"Hide this member's chat only.":"This member left.",other));
   rows.push_back(ConfirmRow("kick","Kick member",m?"Remove "+m->name+" from the room.":"This member left.",host&&other));
   rows.push_back(ConfirmRow("transfer-host","Transfer host",m?"Make "+m->name+" the room host.":"This member left.",host&&other));
 }else if(screen=="room-chat"){
   rows.push_back(TextRow("compose","Compose message",chat_,MaximumChatBytes,mutableRoom));
   rows.push_back(Row("send-chat","Send message","Send the accepted draft. Enter in the editor does not send chat.",mutableRoom&&chat_[0]));
  for(auto it=s.chat.rbegin();it!=s.chat.rend();++it)if(!muted_.count(it->sender))
   rows.push_back(Row("message-"+std::to_string(it->sequence),Name(s,it->sender),it->text));
 }else if(screen=="room-admin"){
  rows.push_back(TextRow("rename","Room name",roomName_,64,host));
  rows.push_back(Value("room-capacity","Capacity",std::to_string(roomCapacity_),"Cannot be smaller than current membership.",host));
  rows.push_back(Value("lock","Admission",s.locked?"Locked":"Open","Lock stops new members joining.",host));
  rows.push_back(Row("apply-name","Apply room name","Submit this room name.",host&&roomName_[0]));
  rows.push_back(Row("apply-capacity","Apply capacity","Submit this capacity.",host&&roomCapacity_>=static_cast<int>(s.members.size())));
 }
 return rows;
}
void ApplicationShell::DrawRoomBoard(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,MenuAction& action,float height) {
 const float s=Scale(),gap=12*s;const bool wide=ImGui::GetContentRegionAvail().x>=820*s;
 const auto focus=nav.Focus();const bool changed=focus!=roomBoardFocus_;
 const auto origin=ImGui::GetCursorScreenPos();
 const float fullHeight=height;
 if(!wide)height=(std::max)(80*s,height-64*s);
 if(focus.compare(0,6,"table-")==0)selectedTable_=std::stoi(focus.substr(6));
 const auto text=[&](ImVec2 p,float width,const std::string& value,float size,ImU32 color){
  if(width<=0)return;
  auto* font=ImGui::GetFont();std::string label=value;
  if(font->CalcTextSizeA(size,FLT_MAX,0,label.c_str()).x>width){
   const char* end=nullptr;const float dots=font->CalcTextSizeA(size,FLT_MAX,0,"...").x;
   font->CalcTextSizeA(size,(std::max)(1.f,width-dots),0,label.c_str(),nullptr,&end);
   label=std::string(label.c_str(),end)+"...";
  }
  ImGui::GetWindowDrawList()->AddText(font,size,p,color,label.c_str());
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
  text(ImVec2(p.x+12*s,p.y+8*s),width*.57f,"BATTLE SLOT "+std::to_string(t.id+1),14*s,palette::Ivory);
  text(ImVec2(p.x+width*.60f,p.y+8*s),width*.40f-12*s,PhaseName(t.phase),12*s,palette::Ember);
  const float top=p.y+28*s,portrait=(std::min)(56*s,h-52*s),half=(width-50*s)*.5f;
  const room::MemberId ids[]={t.p1,t.p2};
  for(int side=0;side<2;++side){
   const float x=p.x+12*s+side*(half+26*s);const auto* m=Member(v.room,ids[side]);const int id=fighter(m);
   if(m)DrawCharacterPortrait(id,ImVec2(x,top),ImVec2(x+portrait,top+portrait));
   const float tx=x+(m?portrait+8*s:0),tw=half-(m?portrait+8*s:0);
   text(ImVec2(tx,top+2*s),tw,m?m->name:"Looking for a fight",16*s,m?palette::Ivory:palette::Muted);
   const auto* f=selection::FindFighter(id);
   std::string caption=m?(f?f->name:"Fighter not shared"):"Open seat";
   const bool ready=t.phase==room::TablePhase::Waiting&&t.ready[side];
   if(m)caption+=ready?" / READY":m->id==v.room.localMember?" / YOU":"";
   text(ImVec2(tx,top+21*s),tw,caption,12*s,ready?IM_COL32(151,197,143,255):palette::Muted);
  }
  text(ImVec2(p.x+width*.5f-12*s,top+18*s),30*s,"VS",16*s,palette::Ember);
  const auto rule=std::to_string(t.rules.roundCount)+" rounds / "+std::to_string(t.rules.roundTime)+" sec";
  text(ImVec2(p.x+12*s,p.y+h-21*s),width-24*s,rule+"     Queue "+std::to_string(t.queue.size())+"     Watching "+std::to_string(t.spectators.size()+t.watchingNext.size()),12*s,palette::Muted);
 };
 const auto memberCard=[&](const MenuEntry& e){
  const auto* m=Member(v.room,std::stoull(e.id.substr(7)));if(!m)return;
  const auto p=ImGui::GetCursorScreenPos();const float width=ImGui::GetContentRegionAvail().x;
  const int main=m->id==v.room.localMember?v.preferences.mainFighter:m->mainFighter;
  press(e,ImVec2(width,58*s));DrawCharacterPortrait(main,ImVec2(p.x+6*s,p.y+5*s),ImVec2(p.x+54*s,p.y+53*s));
  text(ImVec2(p.x+64*s,p.y+7*s),width-76*s,e.label,16*s,palette::Ivory);
  const auto status=std::string(m->host?"HOST / ":"")+StatusName(m->status)+(m->table>=0?" / Slot "+std::to_string(m->table+1):"");
  text(ImVec2(p.x+64*s,p.y+32*s),width-76*s,status,12*s,palette::Muted);
 };
 const auto button=[&](const MenuEntry& e,float width){
  const auto p=ImGui::GetCursorScreenPos();press(e,ImVec2(width,40*s));
  text(ImVec2(p.x+12*s,p.y+11*s),width-24*s,e.label,16*s,e.enabled?palette::Ivory:palette::Muted);
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
  ImGui::Text("MEMBERS  %d / %d",static_cast<int>(v.room.members.size()),v.room.capacity);
  ImGui::BeginChild("Member list",ImVec2(0,(boardHeight-48*s)*.56f),0,ImGuiWindowFlags_NoNavInputs);
  for(const auto& e:rows)if(e.id.compare(0,7,"member-")==0)memberCard(e);
  ImGui::EndChild();ImGui::TextUnformatted("CHAT");
  ImGui::BeginChild("Recent chat",ImVec2(0,0),0,ImGuiWindowFlags_NoNavInputs);
  if(v.room.chat.empty())ImGui::TextWrapped("No messages yet. Select Chat to compose.");
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
  if(e!=rows.end())text(ImGui::GetCursorScreenPos(),ImGui::GetContentRegionAvail().x,e->detail.substr(0,e->detail.find('\n')),12*s,palette::Muted);
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
    explanation=std::string(PhaseName(table.phase))+" / You: "+(local?StatusName(local->status):"Connecting")+
        "\nQueue: "+std::to_string(table.queue.size())+" / Watching: "+std::to_string(table.spectators.size()+table.watchingNext.size());}
   ImGui::PushStyleColor(ImGuiCol_Text,selected->enabled?ToneColor(Tone::Neutral):ToneColor(Tone::Pending));
   ImGui::TextWrapped("%s",explanation.c_str());ImGui::PopStyleColor();
  }
  ImGui::EndChild();
 }
 ImGui::PopStyleColor();ImGui::PopStyleVar();roomBoardFocus_=nav.Focus();
}
void ApplicationShell::RoomAction(const MenuAction& a,const ShellView& v,const Submit& submit) {
 using namespace room;auto& nav=menu_.navigation;
 const auto sendDelay=[&](netplay::CommandKind kind,int selected,bool benchmark=false){
  if(!RoomActionsAvailable(v) || v.delayLocked ||
     (kind==netplay::CommandKind::CheckConnection && (!v.canProbe || v.probeStatus=="checking"))) {
   error_=RoomWaitReason(v);return false;
  }
  ShellAction request;request.command.kind=kind;request.command.generation=v.session.generation;request.selectedDelay=selected;request.command.benchmark=benchmark;
  if(!submit(std::move(request))){error_="The delay action could not be queued. Please try again.";return false;}
  error_.clear();return true;
 };
 if(a.id.compare(0,6,"table-")==0){
  selectedTable_=std::stoi(a.id.substr(6));rulesDirty_=false;rulesRevision_=v.room.tables[selectedTable_].revision;
  tableRules_=v.room.tables[selectedTable_].rules;nav.Push("room-table");return;
 }
 if(a.id.compare(0,7,"member-")==0){selectedMember_=std::stoull(a.id.substr(7));nav.Push("room-member");return;}
 if(a.id=="room-members"||a.id=="room-chat"||a.id=="room-admin"||a.id=="room-rules"){nav.Push(a.id);return;}
  if(a.id=="copy"){ImGui::SetClipboardText(v.invitation.c_str());error_.clear();notice_="Invitation copied.";noticeUntil_=ImGui::GetTime()+3;return;}
  if(a.id=="replace-room"){ShellAction request;request.command.kind=netplay::CommandKind::ReplaceRoom;request.command.generation=v.session.generation;
   if(!submit(std::move(request)))error_="The replacement room could not be queued. Please try again.";else error_.clear();return;}
  if(a.id=="benchmark-connection"){sendDelay(netplay::CommandKind::CheckConnection,-1,true);return;}
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
 else if(a.id=="apply-rules"){request.kind=ActionKind::SetRules;request.rules=tableRules_;}
 else if(a.id=="apply-name"){request.kind=ActionKind::Rename;request.text=roomName_;}
 else if(a.id=="apply-capacity"){request.kind=ActionKind::SetCapacity;request.capacity=static_cast<std::uint8_t>(roomCapacity_);}
 else if(a.id=="lock"){request.kind=ActionKind::Lock;request.locked=a.delta<0;}
 else if(a.id=="send-chat"){request.kind=ActionKind::Chat;request.text=chat_;}
 else if(a.id=="ready"){
  if(!RoomActionsAvailable(v)) { error_=RoomWaitReason(v);return; }
  if(v.room.localTerminalPending || v.room.terminalPending[selectedTable_]) { error_=TerminalPendingReason(); return; }
  const auto* local=Member(v.room,v.room.localMember);
  if(local&&local->seat>=0&&local->seat<2&&v.room.tables[selectedTable_].ready[local->seat]){
   request.kind=ActionKind::Unready;request.seat=local->seat;
  }else{Send(v.session.match==netplay::MatchState::PostMatch?netplay::CommandKind::Rematch:netplay::CommandKind::Ready,v,submit);return;}
 }else return;
 if(SendRoom(std::move(request),v,submit)){
  if(a.id=="send-chat")chat_[0]=0;
  if(a.id=="apply-rules")rulesDirty_=false;
 }else if(error_.empty())error_="The room changed or rejected this action. Review the current state and try again.";
}
} }
