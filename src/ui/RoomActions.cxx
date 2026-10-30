// What the room board's rows and shortcuts do when chosen: the board draws
// them (RoomPanel.cxx), this sends them on.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "RoomControls.hxx"
#include "RoomFeedback.hxx"
#include "../common/Localization.hxx"
#include <imgui.h>

namespace sf4e { namespace ui {
using namespace room_controls;

// X, Y and View open the fighter, table options and chat; the same button
// again returns to the board.
// X, Y and View open the fighter, table options and chat; the same button
// again returns to the board.
void ApplicationShell::RoomShortcut(const MenuAction& a,const ShellView& v) {
 auto& nav=menu_.navigation;
 // Delete means something only on your own card (DrawRoomBoard).
 if(a.delta==MenuInput::Leave)return;
 const char* target=a.delta==MenuInput::Fighter?"selection":a.delta==MenuInput::Options?"room-table":"room-chat";
 if(nav.Screen()==target){nav.Home();nav.Push("room");return;}
 if(a.delta==MenuInput::Fighter){
  const auto blocker=SelectionBlocker(v);
  if(!blocker.empty()){
   Refuse(blocker,[](const ShellView& now){return !SelectionBlocker(now).empty();});
   return;
  }
 }else if(a.delta==MenuInput::Options){
  // The focused table on the board, otherwise the member's own.
  const bool onTable=nav.Screen()=="room"&&(nav.Focus().compare(0,6,"table-")==0||nav.Focus().compare(0,8,"cabinet-")==0);
  OpenTableOptions(v,onTable?selectedTable_:OptionsTable(v,selectedTable_));
  return;
 }
 if(a.delta==MenuInput::Fighter){selectionFresh_=true;selectionOpenOn_="roster";}
 nav.Home();nav.Push("room");nav.Push(target);
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
 // V2: a member's action from their popout. Kick and Make host still ask first, on the member's page.
 if(a.kind==MenuAction::Chosen&&a.id.compare(0,7,"member-")==0){
  selectedMember_=std::stoull(a.id.substr(7));
  if(a.text=="kick"||a.text=="transfer-host"){memberAsk_=a.text;menu_.navigation.Push("room-member");return;}
  RoomAction({MenuAction::Activate,a.text},v,submit);
  return;
 }
 // The V2 board's parts: More…'s switches (which keep it open on them), its picture choice, and each panel's Hide.
 if((a.kind==MenuAction::Chosen&&a.id=="room-more")||a.kind==MenuAction::Activate){
  const std::string& id=a.kind==MenuAction::Chosen?a.text:a.id;auto& p=preferences_;bool changed=true;
  if(id=="room-show-members"||id=="room-hide-members")p.roomShowMembers=id=="room-show-members"&&!p.roomShowMembers;
  else if(id=="room-show-chat"||id=="room-hide-chat")p.roomShowChat=id=="room-show-chat"&&!p.roomShowChat;
  else if(id=="room-hide-picture")p.pictureMode=netplay::PictureOff;
  else if(id.size()==9&&id.compare(0,8,"picture-")==0)p.pictureMode=std::stoi(id.substr(8));
  else if(id=="room-picture"){morePicture_=moreAsk_=true;return;}
  else changed=false;
  if(changed){
   if(a.kind==MenuAction::Chosen&&id.compare(0,10,"room-show-")==0){moreAsk_=true;moreChosen_=id;}
   // A panel's Hide or Show gives the focus to the button that takes its place.
   else if(a.kind==MenuAction::Activate&&id.compare(0,5,"room-")==0&&id!="room-hide-picture")
    menu_.navigation.Prefer(id.compare(0,10,"room-show-")==0?"room-hide-"+id.substr(10):"room-show-"+id.substr(10));
   preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;return;
  }
 }
 // An Arcade room's own cabinet after a set: Rematch or New opponent.
 if(a.kind==MenuAction::Chosen&&(a.text=="rematch"||a.text=="new-opponent")){
  const auto mine=room::PlaceOf(v.room,v.room.localMember);if(mine.kind!=room::Place::Kind::Seat)return;
  Action request;request.table=static_cast<std::uint8_t>(mine.table);request.kind=a.text=="rematch"?ActionKind::Rematch:ActionKind::NewOpponent;
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
  return;
 }
 if(a.kind==MenuAction::Chosen&&a.id.compare(0,8,"cabinet-")==0){if(a.text=="leave-seat")LeavePlace(v,submit);return;}
 // A seat chooser's option, exactly as the player saw it.
 if(a.kind==MenuAction::Chosen){
  // More…'s choice is the row it stands for.
  if(a.id=="more"||a.id=="room-more"){RoomAction({MenuAction::Activate,a.text},v,submit);return;}
  if(a.id.compare(0,6,"table-"))return;
  selectedTable_=std::stoi(a.id.substr(6));
  if(a.text=="options"){OpenTableOptions(v,selectedTable_);return;}
  if(a.text=="leave-seat"){LeavePlace(v,submit);return;}
  const auto* option=FindSeatOption(a.text);
  if(!option)return;
  Action request;request.table=static_cast<std::uint8_t>(selectedTable_);request.kind=option->kind;request.seat=static_cast<std::int8_t>(option->seat);
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
  return;
 }
 if(a.id.compare(0,8,"cabinet-")==0){
  // A free cabinet: Queue names it as table + seat * TableCount.
  const auto cabinet=static_cast<std::size_t>(std::stoi(a.id.substr(8)));
  const auto* m=room::FindMember(v.room,v.room.cabinets[cabinet]);
  if(!m){
   Action request;request.kind=ActionKind::Queue;request.table=static_cast<std::uint8_t>(cabinet%TableCount);request.seat=static_cast<std::int8_t>(cabinet/TableCount);
   if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
   return;
  }
  if(m->table<0)return;
  selectedTable_=m->table;
  if(m->id==v.room.localMember)ToggleReady(v,submit);else OpenTableOptions(v,selectedTable_);
  return;
 }
 if(a.id.compare(0,6,"table-")==0){
  selectedTable_=std::stoi(a.id.substr(6));
  const auto mine=room::PlaceOf(v.room,v.room.localMember);
  // A on your own seat readies; on any other card it opens that table's
  // options, which is looking, not joining: Queue and Watch there keep their
  // own checks.
  if(mine.kind==room::Place::Kind::Seat&&mine.table==selectedTable_)ToggleReady(v,submit);
  else OpenTableOptions(v,selectedTable_);
  return;
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
 if(a.id=="room-members"||a.id=="room-chat"||a.id=="room-admin"){nav.Push(a.id);return;}
 if(a.id=="room-controls"){nav.Push("player");return;}
  if(a.id=="copy"){ImGui::SetClipboardText(v.invitation.c_str());error_.clear();shortCopyPending_=false;ShowNotice(loc::T("room.invitation_copied"),Tone::Success,ImGui::GetTime()+3);return;}
  if(a.id=="copy-short"){CopyShortInvitation(v,submit);return;}
  if(a.id=="copy-room-link"){
   const auto link=publicRooms_.RoomLink();
   if(!link.empty()){ImGui::SetClipboardText(link.c_str());error_.clear();ShowNotice(loc::T("public.link_copied"),Tone::Success,ImGui::GetTime()+3);}
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
  const auto* m=room::FindMember(v.room,selectedMember_);
  if(!m||!m->steamId.Present()){error_=loc::T("room.member_left.detail");return;}
  ShellAction open;open.command.generation=v.session.generation;
  open.service=platform::ServiceAction::OpenSteamProfile;open.steamProfile=m->steamId;
  if(!submit(std::move(open)))error_=loc::T("error.queue_failed");else error_.clear();
  return;
 }
 if(a.id=="mute"){if(muted_.count(selectedMember_))muted_.erase(selectedMember_);else muted_.insert(selectedMember_);return;}
 if(a.id=="rename"){std::snprintf(roomName_,sizeof(roomName_),"%s",a.text.c_str());return;}
 if(a.id=="save-template"){if(!a.text.empty())SaveRoomTemplate(v.room,a.text);return;}
 if(a.id=="table-name"){
  if(a.text==v.room.tables[selectedTable_].name)return;
  Action request;request.table=static_cast<std::uint8_t>(selectedTable_);request.kind=ActionKind::NameTable;request.text=a.text;
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
  return;
 }
 if(a.id=="room-capacity"){roomCapacity_=(std::max)(2,(std::min)(16,roomCapacity_+a.delta));return;}
 if(AdjustRule(tableRules_,a)){rulesDirty_=!(tableRules_==v.room.tables[selectedTable_].rules);return;}
 Action request;request.table=static_cast<std::uint8_t>(selectedTable_);
 if(a.id=="queue")request.kind=ActionKind::Queue;
 else if(a.id=="unqueue")request.kind=ActionKind::Unqueue;
 else if(a.id=="watch")request.kind=ActionKind::Watch;
  else if(a.id=="unwatch")request.kind=ActionKind::Unwatch;
  else if(a.id=="lock-spectating"){
   const auto* local=room::FindMember(v.room,v.room.localMember);
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
