// What the room board's rows and shortcuts do when chosen: the board draws
// them (RoomPanel.cxx), this sends them on.
#include "ApplicationShell.hxx"
#include "MenuRows.hxx"
#include "RoomControls.hxx"
#include "RoomFeedback.hxx"
#include "RowId.hxx"
#include "../common/Localization.hxx"
#include <imgui.h>
#include <algorithm>

namespace sf4e { namespace ui {
using namespace room_controls;
namespace {
bool Has(const std::vector<MenuEntry>& rows,const std::string& id){
 return !id.empty()&&std::any_of(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
}
// Where a step to `area` lands when it remembers nothing there: your own table
// card or cabinet on the floor, Ready on the bar, the first member, the message
// box; else the area's first row. Empty for an area with no rows.
std::string AreaStart(const ShellView& v,const std::vector<MenuEntry>& rows,BoardArea area){
 const auto& s=v.room;
 const auto mine=room::PlaceOf(s,s.localMember);
 std::string first;
 // A panel's Hide is a corner button, not where its area starts.
 for(const auto& e:rows)if(BoardAreaOf(e.id)==area&&!HasPrefix(e.id,"room-hide-")){first=e.id;break;}
 if(area==BoardArea::Floor&&mine.kind!=room::Place::Kind::None){
  const int cabinet=s.arcade?room::CabinetOf(s,s.localMember):-1;
  const std::string own=cabinet>=0?"cabinet-"+std::to_string(cabinet):"table-"+std::to_string(mine.table);
  if(Has(rows,own))return own;
 }
 if(area==BoardArea::Bar&&Has(rows,"ready"))return "ready";
 if(area==BoardArea::Chat&&Has(rows,"room-chat"))return "room-chat";
 return first;
}
}

void ApplicationShell::NoteRoomArea(const std::string& focus) {
 if(!focus.empty())roomAreaFocus_[static_cast<int>(BoardAreaOf(focus))]=focus;
}
// A tab's row: in each of its areas in turn, the row last focused there while
// it is still on the board, else where the area starts.
bool ApplicationShell::FocusRoomTab(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,int tab,bool narrow) {
 for(int i=0;i<static_cast<int>(BoardArea::Count);++i){
  const auto area=static_cast<BoardArea>(i);
  if(BoardTabOf(area,narrow)!=tab)continue;
  const std::string& kept=roomAreaFocus_[i];
  const std::string to=Has(rows,kept)&&BoardAreaOf(kept)==area?kept:AreaStart(v,rows,area);
  if(!to.empty()){nav.Focus(to,rows);return true;}
 }
 return false;
}
// LB and RB: the next area (or the previous; a narrow board's next tab), past
// one with no rows, round at the ends. An open question or popout keeps the
// focus where it is.
void ApplicationShell::StepRoomArea(const ShellView& v,const std::vector<MenuEntry>& rows,MenuNavigation& nav,int step,bool narrow) {
 if(nav.Mode()!=MenuNavigation::Modal::None)return;
 const int count=BoardTabCount(narrow),from=BoardTabOf(BoardAreaOf(nav.Focus()),narrow);
 NoteRoomArea(nav.Focus());
 for(int tab=StepTab(from,step,count);tab!=from;tab=StepTab(tab,step,count))
  if(FocusRoomTab(v,rows,nav,tab,narrow))return;
}
void ApplicationShell::SettleRoomBoard(const ShellView& v,std::vector<MenuEntry>& rows,MenuNavigation& nav) {
 // The action bar's Left and Right go along it in its own order, from the end
 // of one of its lines to the start of the next, and stop at its ends: never
 // off the end into the Members list beside it, whose rows scrolled out of
 // sight are still placed there.
 std::vector<MenuEntry*> bar;
 for(auto& e:rows)if(BoardAreaOf(e.id)==BoardArea::Bar)bar.push_back(&e);
 for(std::size_t i=0;i<bar.size();++i){bar[i]->left=bar[i?i-1:i]->id;bar[i]->right=bar[i+1<bar.size()?i+1:i]->id;}
 // The message box sits under the chat's log, which has nothing to focus:
 // Up from it is its own panel's Hide, not a table card across the board.
 if(Has(rows,"room-hide-chat"))for(auto& e:rows)if(e.id=="room-chat")e.up="room-hide-chat";
 NoteRoomArea(nav.Focus());
 // A room joined opens its board on what the player most likely does next:
 // Ready (Rematch after a set) in a seat, the table they queue at, the bar
 // while watching, else the first table or cabinet with a free seat. Not the last room's row, and not
 // over End practice or Stop watching, which the board opened on over a fight.
 const auto& s=v.room;
 if(!s.roomEpoch||rows.empty()||(roomFocusGeneration_==v.session.generation.room&&roomFocusEpoch_==s.roomEpoch))return;
 roomFocusGeneration_=v.session.generation.room;roomFocusEpoch_=s.roomEpoch;
 for(auto& kept:roomAreaFocus_)kept.clear();
 if(nav.Mode()!=MenuNavigation::Modal::None||nav.Focus()=="end-practice"||nav.Focus()=="stop-watching")return;
 const auto mine=room::PlaceOf(s,s.localMember);
 std::string to=mine.kind==room::Place::Kind::Seat?"ready":mine.kind==room::Place::Kind::Queue?"table-"+std::to_string(mine.table):std::string();
 // Watching, the bar: Stop watching while the game shows, else Table options
 // (a Big picture covers the table cards).
 if(to.empty()&&room::WatchedTable(s,s.localMember)>=0)to=Has(rows,"stop-watching")?"stop-watching":"options";
 if(to.empty()&&s.arcade){
  for(std::size_t c=0;c<room::CabinetCount&&to.empty();++c)
   if(!s.cabinets[c]){const auto id="cabinet-"+std::to_string(c);const auto row=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});if(row!=rows.end()&&row->enabled)to=id;}
 }else if(to.empty()){
  for(std::size_t t=0;t<room::TablesInPlay(s)&&to.empty();++t)
   if((!s.tables[t].p1||!s.tables[t].p2)&&Has(rows,"table-"+std::to_string(s.tables[t].id)))to="table-"+std::to_string(s.tables[t].id);
 }
 // A focus it moves opens like a screen (MenuNavigation::Rearm): that frame
 // counts nothing and a button held into it counts once released, so a press
 // made as a room changes under an open board never lands on the row moved to.
 const std::string& start=Has(rows,to)?to:rows.front().id;
 if(start!=nav.Focus()){nav.Focus(start,rows);nav.Rearm();}
}

// X, Y and View open the fighter, table options and chat; the same button
// again returns to the board.
void ApplicationShell::RoomShortcut(const MenuAction& a,const ShellView& v) {
 auto& nav=menu_.navigation;
 // Only X, Y and View are the room's: Delete means something only on your own
 // card, and LB and RB step the board's areas or tabs (DrawRoomBoard).
 if(a.delta!=MenuInput::Fighter&&a.delta!=MenuInput::Options&&a.delta!=MenuInput::Chat)return;
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
 if(a.delta==MenuInput::Fighter){selectionFresh_=true;selectionOpenOn_=RoomSelectionStart(v.room);}
 nav.Home();nav.Push("room");nav.Push(target);
}
void ApplicationShell::StepPick(const ShellView& v,ShellAction::SelectionStep step,const Submit& submit) {
 ShellAction request;request.command.generation=v.session.generation;request.selectionStep=step;submit(std::move(request));
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
 // Typed text first: a text row's id can share a keyed row's prefix ("table-name"
 // beside "table-0"), so a commit never reaches the numeric rows below (EMBER-68).
 if(a.kind==MenuAction::TextAccepted){
  if(a.id=="rename")std::snprintf(roomName_,sizeof(roomName_),"%s",a.text.c_str());
  else if(a.id=="save-template"){if(!a.text.empty())SaveRoomTemplate(v.room,a.text);}
  // A message written on the keyboard goes as one typed in the box would.
  else if(a.id=="compose"){std::snprintf(chat_,sizeof(chat_),"%s",a.text.c_str());RoomAction({MenuAction::Activate,"compose"},v,submit);}
  else if(a.id=="table-name"&&a.text!=v.room.tables[selectedTable_].name){
   Action request;request.table=static_cast<std::uint8_t>(selectedTable_);request.kind=ActionKind::NameTable;request.text=a.text;
   if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
  }
  return;
 }
 // A member's action from their popout. Kick and Make host still ask first, on the member's page.
 if(const auto member=RowKey(a.id,"member-");member&&a.kind==MenuAction::Chosen){
  selectedMember_=*member;
  if(a.text=="kick"||a.text=="transfer-host"){memberAsk_=a.text;menu_.navigation.Push("room-member");return;}
  RoomAction({MenuAction::Activate,a.text},v,submit);
  return;
 }
 // The board's parts: More…'s switches (which keep it open on them), its
 // picture choice, and each panel's Hide. They are the board's layout, which
 // changes only while the room leaves its settings open (ShellView::CanEditRoomSettings).
 if((a.kind==MenuAction::Chosen&&a.id=="room-more")||a.kind==MenuAction::Activate){
  const std::string& id=a.kind==MenuAction::Chosen?a.text:a.id;auto p=preferences_;bool changed=true;
  if(id=="room-show-members"||id=="room-hide-members")p.roomShowMembers=id=="room-show-members"&&!p.roomShowMembers;
  else if(id=="room-show-chat"||id=="room-hide-chat")p.roomShowChat=id=="room-show-chat"&&!p.roomShowChat;
  else if(id=="room-hide-picture")p.pictureMode=netplay::PictureOff;
  else if(const auto mode=RowKey<int>(id,"picture-");mode&&*mode<=netplay::PictureOff)p.pictureMode=*mode;
  else if(id=="room-picture"){morePicture_=moreAsk_=true;return;}
  else changed=false;
  if(changed){
   if(!v.CanEditRoomSettings())return;
   preferences_=p;
   if(a.kind==MenuAction::Chosen&&id.compare(0,10,"room-show-")==0){moreAsk_=true;moreChosen_=id;}
   // A panel's Hide or Show gives the focus to the button that takes its place.
   else if(a.kind==MenuAction::Activate&&id.compare(0,5,"room-")==0&&id!="room-hide-picture")
    menu_.navigation.Prefer(id.compare(0,10,"room-show-")==0?"room-hide-"+id.substr(10):"room-show-"+id.substr(10));
   preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;return;
  }
 }
 // An Arcade pair's answer after a set, Rematch or New opponent: one of its
 // own cabinet's choices, or the post-match prompt's button of the same id.
 if(a.kind==MenuAction::Activate&&(a.id=="rematch"||a.id=="new-opponent")){
  const auto mine=room::PlaceOf(v.room,v.room.localMember);if(mine.kind!=room::Place::Kind::Seat)return;
  Action request;request.table=static_cast<std::uint8_t>(mine.table);request.kind=a.id=="rematch"?ActionKind::Rematch:ActionKind::NewOpponent;
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
  return;
 }
 // A cabinet's choice is the row it stands for; Leave seat answers its question.
 if(a.kind==MenuAction::Chosen&&RowKey(a.id,"cabinet-")){
  if(a.text=="leave-seat")LeavePlace(v,submit);else if(a.text!="stay")RoomAction({MenuAction::Activate,a.text},v,submit);
  return;
 }
 // A seat chooser's option, exactly as the player saw it.
 if(a.kind==MenuAction::Chosen){
  // More…'s choice is the row it stands for.
  if(a.id=="room-more"){RoomAction({MenuAction::Activate,a.text},v,submit);return;}
  const auto table=RowKey<int>(a.id,"table-");if(!table||*table>=static_cast<int>(v.room.tables.size()))return;
  selectedTable_=*table;
  if(a.text=="options"){OpenTableOptions(v,selectedTable_);return;}
  if(a.text=="leave-seat"){LeavePlace(v,submit);return;}
  const auto* option=FindSeatOption(a.text);
  if(!option)return;
  Action request;request.table=static_cast<std::uint8_t>(selectedTable_);request.kind=option->kind;request.seat=static_cast<std::int8_t>(option->seat);
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
  return;
 }
 if(const auto index=RowKey<std::size_t>(a.id,"cabinet-");index&&*index<room::CabinetCount){
  // A free cabinet: Queue names it as table + seat * TableCount.
  const auto cabinet=*index;
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
 // A matchup under the cabinets opens its pair's table options.
 if(const auto table=RowKey<int>(a.id,"matchup-");table&&*table<static_cast<int>(v.room.tables.size())){OpenTableOptions(v,*table);return;}
 if(const auto table=RowKey<int>(a.id,"table-");table&&*table<static_cast<int>(v.room.tables.size())){
  selectedTable_=*table;
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
 if(a.id=="follow-room"){ToggleFollow(v,submit);return;}
 if(a.id=="leave-place"){
  const auto exit=ExitFromPlace(v);if(exit.table<0)return;
  selectedTable_=exit.table;
  // A seat that would lose a score or pass to the queue is asked about first;
  // the board opens the question on the next frame, with the answer in Chosen.
  if(exit.seat&&exit.allowed&&exit.cost){leaveAsk_=exit.table;leaveAsked_=false;return;}
  LeavePlace(v,submit);
  return;
 }
 if(const auto member=RowKey(a.id,"member-")){selectedMember_=*member;nav.Push("room-member");return;}
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
  if(a.id=="ultra"||a.id=="room-ultra"||a.id=="appearance"){
   // Select shows the Ultra or costume cards (a costume goes on to its
   // colors), and picking one comes back here: from the table options' rows,
   // and from the bar's Ultra beside Ready.
   const bool ultra=a.id!="appearance";
   if(a.kind==MenuAction::Adjust){
    if(v.canEditSelection&&RoomActionsAvailable(v))
     StepPick(v,{ultra?ShellAction::SelectionStep::Field::Ultra:ShellAction::SelectionStep::Field::Color,a.delta},submit);
   }else if(SelectionBlocker(v).empty()){selectionFresh_=true;selectionOpenOn_=ultra?SelectionStart::Ultras:SelectionStart::Costumes;nav.Push("selection");}
   return;
  }
  if(a.id=="stage"||a.id=="fighter-options"){
   if(a.kind==MenuAction::Activate&&SelectionBlocker(v).empty()){selectionFresh_=true;selectionOpenOn_=a.id=="stage"?SelectionStart::Stage:SelectionStart::Options;nav.Push("selection");}
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
 if(a.id=="room-capacity"){roomCapacity_=(std::max)(2,(std::min)(16,roomCapacity_+a.delta));return;}
 if(a.id=="room-tables"){roomTableCount_=(std::max)(1,(std::min)(static_cast<int>(TableCount),roomTableCount_+a.delta));return;}
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
 else if(a.id=="apply-tables"){
  // A table taken away must be empty; the room says so too, but nothing is sent that it would refuse.
  if(!room::TableCountAllowed(v.room,static_cast<std::size_t>(roomTableCount_))){error_=loc::T("room.action_rejected");return;}
  request.kind=ActionKind::SetTableCount;request.tableCount=static_cast<std::uint8_t>(roomTableCount_);
 }
 else if(a.id=="lock"){request.kind=ActionKind::Lock;request.locked=a.delta<0;}
 else if(a.id=="room-wired-only"){
  // Left is off and Right on; Select turns it the other way.
  const bool on=a.kind==MenuAction::Adjust?a.delta>0:!v.room.wiredOnly;
  if(on==v.room.wiredOnly)return;
  request.kind=ActionKind::SetWiredOnly;request.wiredOnly=on;
 }
 else if(a.id=="room-tiny-fighters"){
  // Left is off and Right on; Select turns it the other way.
  const bool on=a.kind==MenuAction::Adjust?a.delta>0:!v.room.tinyFighters;
  if(on==v.room.tinyFighters)return;
  request.kind=ActionKind::SetTinyFighters;request.tinyFighters=on;
 }
 else if(a.id=="room-skip-scenes"){
  // Left is off and Right on; Select turns it the other way.
  const bool on=a.kind==MenuAction::Adjust?a.delta>0:!v.room.skipScenes;
  if(on==v.room.skipScenes)return;
  request.kind=ActionKind::SetSkipScenes;request.skipScenes=on;
 }
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
// Follow the room's row: on the board's action bar while watching, and in a
// table's options beside Watch. A lock-in holds one table, so it is released
// first; following never makes one.
MenuEntry ApplicationShell::FollowRow(const ShellView& v) const {
 if(follow_.On()){
  const int table=follow_.Followed(v.room);
  return Row("follow-room",loc::T("room.follow.stop"),table<0?std::string(loc::T("room.follow.detail")):loc::Tf("room.follow.stop_detail",table+1));
 }
 const auto* local=room::FindMember(v.room,v.room.localMember);
 if(local&&local->spectatorLocked)return Row("follow-room",loc::T("room.follow"),loc::T("room.follow.locked_in"),false);
 const bool open=RoomActionsAvailable(v);
 return Row("follow-room",loc::T("room.follow"),open?std::string(loc::T("room.follow.detail")):RoomWaitReason(v),open);
}
// Following starts where the member watches. One that watches nothing first
// watches the table whose options it is on, and that Watch is the first move.
void ApplicationShell::ToggleFollow(const ShellView& v,const Submit& submit) {
 if(follow_.On()){follow_.Stop();return;}
 const auto row=FollowRow(v);
 if(!row.enabled){Refuse(row.detail);return;}
 follow_.Start(v.room);
 if(room::WatchedTable(v.room,v.room.localMember)>=0)return;
 room::Action watch;watch.kind=room::ActionKind::Watch;watch.table=static_cast<std::uint8_t>(selectedTable_);
 if(!SendRoom(std::move(watch),v,submit)){follow_.Stop();if(error_.empty())error_=loc::T("room.action_rejected");}
}
// Every Watch, Unwatch and lock-in SendRoom sends, the member's own or
// following's, is the member's one watching intent (room::RoomFollow): a
// Stop watching ends following at once and holds, whatever move is still on
// its way, until the room shows the member watching nothing.
void ApplicationShell::NoteWatching(const room::Action& sent,const room::Snapshot& room) {
 const auto nowMs=static_cast<std::uint64_t>(ImGui::GetTime()*1000);
 if(sent.kind==room::ActionKind::Watch)follow_.Watched(sent.table,nowMs);
 else if(sent.kind==room::ActionKind::Unwatch)follow_.StoppedWatching(room,sent.table,nowMs);
 else if(sent.kind==room::ActionKind::LockSpectating&&sent.locked)follow_.Stop();
}
// After the frame's own presses, drawn or hidden (Background), the watching
// intent decides from the room as this PC has it. A move waits for a room that
// takes it and for this PC's watched game to finish showing. A step that did
// not go is noted as sent, so it waits as one the room refused and goes again.
void ApplicationShell::UpdateFollow(const ShellView& v,double now,const Submit& submit) {
 const auto nowMs=static_cast<std::uint64_t>(now*1000);
 const auto step=follow_.Next(v.room,nowMs,{RoomActionsAvailable(v),v.watchedGame.Active()});
 if(!step)return;
 room::Action action;action.kind=step.kind;action.table=static_cast<std::uint8_t>(step.table);
 if(!SendRoom(room::Action(action),v,submit))NoteWatching(action,v.room);
}
} }
