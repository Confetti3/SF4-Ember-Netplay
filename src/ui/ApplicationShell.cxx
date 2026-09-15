#include "ApplicationShell.hxx"
#include "Theme.hxx"
#include "MenuRows.hxx"
#include "RoomFeedback.hxx"
#include "../common/FighterCatalog.hxx"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <utility>
namespace sf4e { namespace ui {
bool ApplicationShell::Service(platform::ServiceAction kind, const ShellView& view, const Submit& submit) {
    ShellAction action; action.service = kind; action.command.generation = view.session.generation;
    if (!submit(std::move(action))) { error_ = "The operation is busy. Please try again."; return false; }
    error_.clear(); return true;
}
bool ApplicationShell::Send(netplay::CommandKind kind, const ShellView& view, const Submit& submit) {
    ShellAction action;
    action.command.kind = kind;
    action.command.generation = view.session.generation;
    action.preferences = preferences_;
    if (kind == netplay::CommandKind::SetLobbySettings) action.preferences.lobby = lobby_;
    else if (kind == netplay::CommandKind::HostRoom || kind == netplay::CommandKind::SavePreferences) {
        // Keep the legacy lobby copy synchronized while the room defaults use
        // the richer per-table Rules contract.
        action.preferences.tableRules.format = room::SetFormat::Unlimited;
        action.preferences.tableRules.rotation = room::RotationMode::WinnerStays;
        action.preferences.lobby.editionSelect = action.preferences.tableRules.editionSelect;
        action.preferences.lobby.roundCount = action.preferences.tableRules.roundCount;
        action.preferences.lobby.roundTime = action.preferences.tableRules.roundTime;
    }
    if (kind == netplay::CommandKind::JoinInvite) action.command.invitation = invitation_;
    if (!submit(std::move(action))) { error_ = "Could not accept the action. Please try again."; return false; }
    error_.clear();
    if (kind == netplay::CommandKind::JoinInvite || kind == netplay::CommandKind::LeaveRoom)
        std::fill(std::begin(invitation_), std::end(invitation_), '\0');
    return true;
}


void ApplicationShell::Draw(const ShellView& v,bool* open,const Submit& submit,const DrawSelection& selection,const DrawSelection& developer) {
 using namespace netplay; auto& nav=menu_.navigation;
 const double now = ImGui::GetTime();
 if(lastUiTime_ >= 0 && now < lastUiTime_) {
  // DX9 reset recreates ImGui, but these deadlines belong to the surviving shell.
  // Keep raw-clock users in RoomAction in the same epoch, including queued saves.
  saveAt_ = RebaseUiTimestamp(saveAt_, lastUiTime_, now);
  noticeUntil_ = RebaseUiTimestamp(noticeUntil_, lastUiTime_, now);
  roomUpdateUntil_ = RebaseUiTimestamp(roomUpdateUntil_, lastUiTime_, now);
  roomUpdateStarted_ = -1;
 }
 lastUiTime_ = now;
 if(previousRoomState_!=RoomState::Idle && v.session.room==RoomState::Idle) {
  nav.Cancel();
  if(nav.Screen().compare(0,4,"room")==0 || nav.Screen()=="selection")nav.Home();
  error_.clear();
  notice_=previousRoomState_==RoomState::Opening?"":"You left the room.";
  noticeUntil_=now+3;
 }
 previousRoomState_=v.session.room;
 if(!(generation_==v.session.generation)) {
  const bool roomChanged=generation_.room!=v.session.generation.room;
  generation_=v.session.generation; nav.Cancel(); error_.clear();notice_.clear();
  if(roomChanged&&v.session.room!=RoomState::Idle&&nav.Screen()!="room"){nav.Home();nav.Push("room");}
  if(v.session.room==RoomState::Idle&&nav.Screen().compare(0,4,"room")==0)nav.Home();
  if(roomChanged){roomUpdateUntil_=0;roomUpdateStarted_=-1;roomDetails_.clear();}
 }
 // Ignore brief checkpoint delays; hold visible feedback through short gaps.
 // Eligibility still uses the current snapshot on every frame.
 if(v.room.roomEpoch!=roomEpoch_){roomUpdateUntil_=0;roomUpdateStarted_=-1;roomDetails_.clear();}
 const bool healthyRoom=v.session.room==RoomState::Joined&&v.session.control==Health::Healthy&&
  v.session.recovery==Recovery::None&&!v.room.closed;
 if(!healthyRoom){roomUpdateUntil_=0;roomUpdateStarted_=-1;roomDetails_.clear();}
 else if(RoomCheckpointPending(v)) {
  if(roomUpdateStarted_<0)roomUpdateStarted_=ImGui::GetTime();
  if(ImGui::GetTime()-roomUpdateStarted_>=.25||ImGui::GetTime()<roomUpdateUntil_)
   roomUpdateUntil_=ImGui::GetTime()+.5;
 }else roomUpdateStarted_=-1;
 roomUpdateVisible_=healthyRoom&&ImGui::GetTime()<roomUpdateUntil_;
 if(saveQueued_&&!v.settingsPending){
  if(SamePreferences(v.preferences,savingPreferences_)&&v.settingsError.empty()){
   saveQueued_=false;retrySave_=false;preferencesDirty_=!SamePreferences(preferences_,savingPreferences_);
   if(profileSavePending_&&!preferencesDirty_){
    profileSavePending_=false;error_.clear();notice_="Profile portrait saved: "+std::string(selection::FindFighter(v.preferences.mainFighter)->name);noticeUntil_=ImGui::GetTime()+3;
    if(nav.Screen()=="main-character")nav.Return();
   }
  }else if(ImGui::GetTime()>saveAt_+2){saveQueued_=false;saveFailed_=true;retrySave_=false;error_="Settings were not saved. Retry when the menu is available.";}
 }
 if(!preferencesDirty_&&!saveQueued_&&!saveFailed_&&!v.settingsPending)preferences_=v.preferences;
 if(!v.settingsError.empty()&&!retrySave_&&!saveQueued_)saveFailed_=true;
 if(preferencesDirty_&&!saveQueued_&&!saveFailed_&&!v.settingsPending&&v.canEditPreferences&&preferences_.Valid()&&ImGui::GetTime()>=saveAt_){
  if(Send(CommandKind::SavePreferences,v,submit)){saveQueued_=true;savingPreferences_=preferences_;saveAt_=ImGui::GetTime();}else saveFailed_=true;
 }
 if(v.discordPending&&inviteRevision_!=v.discordRevision){inviteRevision_=v.discordRevision;nav.Push("discord-invitation");}
 if(!v.discordPending&&nav.Screen()=="discord-invitation")nav.Return();
 if(v.inputCapture!=input::Capture::Idle&&nav.Screen()!="assignment")nav.Push("assignment");
 if(v.inputCapture==input::Capture::Idle&&nav.Screen()=="assignment")nav.Return();
 const auto* vp=ImGui::GetMainViewport();ImGui::SetNextWindowPos(vp->Pos);ImGui::SetNextWindowSize(vp->Size);
 ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20*Scale(),16*Scale()));ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
 ImGui::Begin("SF4 Ember Netplay###EmberShell",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs);
 if(nav.Screen()=="selection"&&selection){
  selection();
  if(TakeMenuReturn())nav.Return();ImGui::End();ImGui::PopStyleVar(2);return;
 }
 const std::string screen=nav.Screen();std::vector<MenuEntry> rows;std::string title="SF4 EMBER";
 const bool idle=v.session.room==RoomState::Idle;
 const char* reason=v.canEditPreferences?"Changes save automatically.":"Leave the room to change personal settings.";
 if(v.inputCapture!=input::Capture::Idle){
  title="ASSIGN CONTROLLER";rows={Row("capture-cancel","Cancel controller assignment",v.inputCapture==input::Capture::Press?"Press a button on your chosen controller.":"Release all buttons to continue.")};
 }else if(screen=="home"){
  rows={Row("online","Online Play",idle?"Create a private room or join an invitation.":"Return to your current room."),
   Row("selection","Fighter Select",v.canEditSelection?v.selectionSummary:
    v.selectionLockReason.empty()?"Return to your table for the current selection status.":v.selectionLockReason,bool(selection)),
   Row("profile","Profile","Your name, main character and local confirmed online record."),
   Row("settings","Settings","Player, controller, gameplay defaults, interface and Discord."),
   Row("about","Help & About","Controls, diagnostics, updates and credits."),
   Row("offline","Play Offline","Open the native offline menus.",idle)};
  if(!v.controllerReady)rows.insert(rows.begin(),Row("player","Choose gameplay controller","Explicit assignment is required before online play."));
  if(developer)rows.push_back(Row("developer","Developer","Development tools."));
 }else if(screen=="profile"){
  title="PROFILE";const auto& record=v.preferences.record;
  rows={TextRow("name","Player name",preferences_.displayName,31,v.canEditPreferences),
   Row("main-character","Main character",std::string(selection::FindFighter(preferences_.mainFighter)->name)+". Used for your player card, independently of match selection."),
   Row("record","Win / loss record",record.available?std::to_string(record.wins)+" wins / "+std::to_string(record.losses)+" losses. Local confirmed online games since tracking began; draws, cancellations, offline games and unresolved results are excluded.":"Record unavailable. The original settings are preserved.")};
  rows[1].value=selection::FindFighter(preferences_.mainFighter)->name;
  rows[2].value=record.available?std::to_string(record.wins)+" W / "+std::to_string(record.losses)+" L":"Unavailable";
 }else if(screen=="main-character"){
  title="CHOOSE YOUR MAIN";
  for(int id=0;id<selection::FighterCount;++id)rows.push_back(Row("main-"+std::to_string(id),selection::FindFighter(id)->name,v.canEditPreferences?"Select to save your main and update your player card.":"Leave the room to edit your profile.",v.canEditPreferences));
 }else if(screen=="online"){
  title="ONLINE PLAY";rows={Row("create","Create Room","Set up a private room.",v.canOpenRoom),Row("join","Join Room","Paste an invitation.",v.canOpenRoom)};
 }else if(screen=="create"||screen=="defaults"){
  title=screen=="create"?"CREATE ROOM":"GAMEPLAY DEFAULTS";const bool can=screen=="create"?v.canOpenRoom:v.canEditPreferences;
  if(screen=="defaults")rows.push_back(Value("delay","Input delay",std::to_string(preferences_.inputDelay),reason,can));
  rows.push_back(TextRow("room-name","Room name",preferences_.roomName,64,can));
  rows.push_back(Value("capacity","Capacity",std::to_string(preferences_.roomCapacity),"Maximum members, including spectators.",can));
  RuleRows(rows,preferences_.tableRules,can,reason);
  if(screen=="create")rows.push_back(Row("host","Create Room","Networking and an assigned gameplay controller must be ready.",can&&preferences_.Valid()));
 }else if(screen=="join"){
  title="JOIN ROOM";rows={Row("paste","Paste Invitation","Copy an Ember invitation, then select this row.",v.canOpenRoom),
   Row("join-now","Join","Join using the pasted invitation.",v.canOpenRoom&&invitation_[0]),
   TextRow("invite-text","Edit invitation",invitation_,sizeof(invitation_)-1,v.canOpenRoom)};
 }else if(screen.compare(0,4,"room")==0){title=v.room.name.empty()?"ROOM":v.room.name;rows=RoomEntries(v);
 }else if(screen=="settings"){
  title="SETTINGS";rows={Row("player","Player & Controller","Display name and controller assignment."),Row("defaults","Gameplay Defaults","Input delay and new-room defaults."),Row("interface","Interface","Quiet HUD and interface size."),Row("discord","Discord","Activity and invitations.")};
 }else if(screen=="player"){
  title="PLAYER & CONTROLLER";rows={TextRow("name","Display name",preferences_.displayName,31,v.canEditPreferences),
   Row("capture","Change controller",v.controller+". Release, press, then release to assign.",v.canChangeController),
   Row("keyboard","Use keyboard","Explicitly assign the keyboard.",v.canChangeController),
   Row("controls","Open native menus","Ember will close. Choose Options in the game menu to configure fighting buttons.",idle)};
 }else if(screen=="interface"){
  title="INTERFACE";char size[32];std::snprintf(size,sizeof(size),"%.2fx",preferences_.interfaceScale);
  const char* hudSizes[]={"Small","Standard","Large"};
  rows={Value("hud","Match HUD",preferences_.showMatchHud?"On":"Off",reason,v.canEditPreferences),
   Value("hud-size","Match HUD size",hudSizes[(std::max)(0,(std::min)(2,preferences_.matchHudSize))],"Changes in-match text and panel size.",v.canEditPreferences),
   Value("hud-spacing","Bottom spacing",preferences_.matchHudRaised?"Raised":"Normal","Moves the match HUD above the bottom edge.",v.canEditPreferences),
   Value("scale","Interface size",size,reason,v.canEditPreferences)};
 }else if(screen=="discord"){
  title="DISCORD";rows={Value("presence","Show activity",preferences_.discordPresence?"On":"Off",v.discordStatus,v.canEditPreferences),
   Value("invites","Allow invitations",preferences_.discordInvites?"On":"Off",preferences_.discordPresence?"Invitations share access to your room.":"Enable Discord activity first.",v.canEditPreferences&&preferences_.discordPresence)};
 }else if(screen=="discord-invitation"){
  title="DISCORD INVITATION";rows={Row("invite-cancel","Cancel invitation","Discard the invitation without leaving your current room.")};
  if(v.discordConfirm)rows.push_back(ConfirmRow("invite-switch","Switch to invited room",v.discordCanSwitch?"Leave this room and join the invitation.":"Wait for the current game to finish.",v.discordCanSwitch));
  else rows.push_back(Row("invite-wait","Invitation pending","Waiting for the main menu, network, and assigned controller.",false));
 }else if(screen=="developer"&&developer){developer();if(ImGui::Button("Back to Home"))nav.Return();
 }else{
  title="HELP & ABOUT";rows={Row("help","Controls","Xbox: A selects; B returns, independently of fighting bindings. DirectInput: mapped LP selects; LK returns. D-pad moves; Left/Right adjusts. Training controls use keyboard and mouse: F6 opens/closes, arrows navigate, Enter selects, Escape goes back. Start only opens native pause in training. F10 opens Ember at the main menu; F5-F8 are training shortcuts."),
   Row("credits","About Ember","Unofficial Ultra Street Fighter IV mod, based on sf4e by Anthony Danducci. Iroh / QUIC, GGPO, Dear ImGui and Inter typography. Build: "+v.build),
   Row("font","Font license",FontLicense()),Row("diagnostics","Export diagnostics","Exports connection states without invitations or credentials.",!v.services.pending),
   Row("updates","Check for updates",v.services.message,!v.services.pending)};
  if(v.services.update.ok&&v.services.update.updateAvailable)rows.push_back(ConfirmRow("updater","Exit and open updater","The game will close. Leave your room first.",v.canEditPreferences&&!v.services.pending));
  if(v.network==NetworkAvailability::Unavailable)rows.push_back(ConfirmRow("recovery","Exit to recovery","Close the game and open recovery.",!v.services.pending));
 }
 if(saveFailed_)rows.push_back(Row("retry-save","Retry saving",v.settingsError.empty()?error_:v.settingsError,v.canEditPreferences&&!v.settingsPending));
 const bool personal=screen=="profile"||screen=="main-character"||screen=="settings"||screen=="player"||screen=="defaults"||screen=="interface"||screen=="discord";
 std::string status=saveFailed_?"Save failed":v.settingsPending||preferencesDirty_||saveQueued_?"Saving...":personal?"Saved":"";
 if(screen=="room"&&status.empty())status=v.session.control==Health::Healthy?(v.room.locked?"Room locked / invitation only":"Private room / invitation only"):"Reconnecting to room...";
 if(screen=="room-table"){
  title="TABLE "+std::to_string(selectedTable_+1)+" / BATTLE SETUP";
  const auto& table=v.room.tables[selectedTable_];
  const auto state=[&](room::MemberId id,int side){
   if(!id)return std::string("Waiting for opponent");
   const auto member=std::find_if(v.room.members.begin(),v.room.members.end(),[&](const room::Member& m){return m.id==id;});
   return (id==v.room.localMember?std::string("You"):member==v.room.members.end()?std::string("Player"):member->name)+
    (table.ready[side]?" - READY":" - Not ready");
  };
  status=state(table.p1,0)+" | "+state(table.p2,1);
  if(table.phase==room::TablePhase::Paused)status="Result unresolved / host can cancel the previous game";
  else if(table.phase==room::TablePhase::Ready)status="Preparing match / fighter choices locked";
  else if(table.phase==room::TablePhase::Playing)status=table.resultPending||
   (v.session.match==MatchState::PostMatch&&(table.p1==v.room.localMember||table.p2==v.room.localMember))?
   "Waiting for results / next match is not ready":"Match in progress";
  else if(table.phase==room::TablePhase::Closed)status="Table closed";
 }
 if(ImGui::GetTime()>=noticeUntil_)notice_.clear();
 if(!notice_.empty()&&!saveFailed_&&!v.settingsPending&&!preferencesDirty_&&!saveQueued_)status=notice_;
 if(v.controllerUnavailable)status="Controller disconnected. Reconnect or assign explicitly.";
 if(!v.session.error.empty())status=v.session.error;if(!v.error.empty())status=v.error;if(!error_.empty())status=error_;
 const bool roomScreen=screen.compare(0,4,"room")==0;
 if(roomScreen&&v.session.recovery!=Recovery::None)status=!v.session.error.empty()?v.session.error:
  v.session.recovery==Recovery::ReplacementOffered?"Room control is unavailable. Replace the room when no match is active.":
  "Room control is recovering. Room actions are paused.";
 const auto tablePhase=v.room.tables[selectedTable_].phase;
 const bool committedMatchStatus=screen=="room-table" && healthyRoom &&
  (tablePhase==room::TablePhase::Ready || tablePhase==room::TablePhase::Playing || tablePhase==room::TablePhase::Paused);
 if(roomScreen && (v.session.room==RoomState::Closing ||
    (v.session.control==Health::Healthy && v.session.recovery==Recovery::None &&
     ((!RoomActionsAvailable(v)&&!RoomCheckpointPending(v))||roomUpdateVisible_) && !committedMatchStatus &&
     !v.controllerUnavailable&&v.session.error.empty()&&v.error.empty()&&error_.empty())))
  status=RoomWaitReason(v);
 PlayerCardView card;card.name=preferences_.displayName;card.fighter=preferences_.mainFighter;
 card.fighterName=selection::FindFighter(preferences_.mainFighter)->name;card.inputDelay=preferences_.inputDelay;
 card.wins=v.preferences.record.wins;card.losses=v.preferences.record.losses;card.recordAvailable=v.preferences.record.available;
 card.controllerReady=v.controllerReady;card.connected=v.session.control==Health::Healthy;
 card.members=static_cast<int>(v.room.members.size());
 for(const auto& table:v.room.tables)if(table.phase==room::TablePhase::Playing)++card.activeTables;
 SetMenuPlayerCard(std::move(card));
 const int columns=screen=="main-character"?(std::max)(3,(std::min)(8,static_cast<int>(ImGui::GetContentRegionAvail().x/(170*Scale())))):1;
 GameMenu::Card portraits;
 if(screen=="main-character")portraits=[&](const MenuEntry& e,ImVec2 min,ImVec2 max){
  if(e.id.compare(0,5,"main-")!=0)return false;
  const int id=std::stoi(e.id.substr(5));DrawMainPortrait(id,id==v.preferences.mainFighter,min,max);return true;
 };
 GameMenu::Detail profilePreview;
 if(screen=="profile"||screen=="main-character")profilePreview=[&](const std::string& id){
  const int fighter=screen=="main-character"&&id.compare(0,5,"main-")==0?std::stoi(id.substr(5)):v.preferences.mainFighter;
  const auto space=ImGui::GetContentRegionAvail();const float size=(std::min)(220*Scale(),(std::min)(space.x,space.y-12*Scale()));
  if(size>=48*Scale()){const auto p=ImGui::GetCursorScreenPos();DrawCharacterPortrait(fighter,p,ImVec2(p.x+size,p.y+size));ImGui::Dummy(ImVec2(size,size));}
 };
 GameMenu::Body board;
 if(screen=="interface")profilePreview=[&](const std::string&){
  ImGui::TextUnformatted("Preview");
  MatchStripView preview;preview.names[0]="Player One";preview.names[1]="Player Two";
  preview.pingMs=68;preview.rollbackFrames=2;preview.appliedDelay=3;
  preview.size=preferences_.matchHudSize;preview.raised=preferences_.matchHudRaised;
  DrawMatchStripPreview(preview);
 };
 if(screen=="room"&&v.room.roomEpoch)board=[&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction& action,float height){DrawRoomBoard(v,entries,navigation,action,height);};
 // Visual grace cannot grant permission: enabled and all dispatch checks stay live.
 const bool checkpointPending=roomScreen && RoomCheckpointPending(v) && !v.controllerUnavailable &&
  v.session.error.empty() && v.error.empty() && error_.empty();
 for(auto& row:rows)row.pending=checkpointPending;
 const bool stableFeedback=roomScreen || personal || screen=="join" || screen=="create" || screen=="about";
 auto a=menu_.Draw(title.c_str(),rows,status.c_str(),profilePreview,columns,portraits,board,0,100,stableFeedback);
 if(v.inputCapture!=input::Capture::Idle&&(a.id=="capture-cancel"||a.kind==MenuAction::Returned||a.kind==MenuAction::Close)){
  ShellAction r;r.command.generation=v.session.generation;r.inputAction=input::Action::Cancel;submit(std::move(r));
 }else if(a.kind==MenuAction::Close||a.id=="return"){if(open)*open=false;
 }else if(a.kind==MenuAction::Activate){
  if(a.id=="online")nav.Push(idle?"online":"room");
  else if(a.id=="profile"||a.id=="main-character")nav.Push(a.id);
  else if(a.id.compare(0,5,"main-")==0&&v.canEditPreferences){preferences_.mainFighter=std::stoi(a.id.substr(5));preferencesDirty_=true;profileSavePending_=true;error_.clear();saveAt_=ImGui::GetTime()+.45;}
  else if(a.id=="selection"||a.id=="settings"||a.id=="about"||a.id=="create"||a.id=="join"||a.id=="player"||a.id=="defaults"||a.id=="interface"||a.id=="discord"||a.id=="developer")nav.Push(a.id);
  else if(a.id=="host"||a.id=="join-now"){if(Send(a.id=="host"?CommandKind::HostRoom:CommandKind::JoinInvite,v,submit)){nav.Home();nav.Push("room");}}
  else if(a.id=="offline"||a.id=="controls")Send(CommandKind::StartOffline,v,submit);
  else if(a.id=="paste"){const char* t=ImGui::GetClipboardText();if(t&&*t&&std::strlen(t)<sizeof(invitation_)){std::strcpy(invitation_,t);error_.clear();}else error_="Invitation is empty or too long.";}
  else if(a.id=="capture"||a.id=="keyboard"){ShellAction r;r.command.generation=v.session.generation;r.inputAction=a.id=="capture"?input::Action::BeginCapture:input::Action::UseKeyboard;submit(std::move(r));}
  else if(a.id=="invite-cancel"||a.id=="invite-switch"){ShellAction r;r.command.generation=v.session.generation;r.discordRevision=v.discordRevision;r.discordAction=a.id=="invite-cancel"?discord::InviteAction::Cancel:discord::InviteAction::Switch;if(!submit(std::move(r)))error_="Invitation changed. Try again.";}
  else if(a.id=="retry-save"){saveFailed_=false;retrySave_=true;preferencesDirty_=true;saveAt_=0;error_.clear();}
  else if(a.id=="diagnostics")Service(platform::ServiceAction::ExportDiagnostics,v,submit);
  else if(a.id=="updates")Service(platform::ServiceAction::CheckUpdates,v,submit);
  else if(a.id=="updater")Service(platform::ServiceAction::OpenUpdater,v,submit);
  else if(a.id=="recovery")Service(platform::ServiceAction::OpenRecovery,v,submit);
  else if(screen.compare(0,4,"room")==0)RoomAction(a,v,submit);
 }else if(a.kind==MenuAction::Adjust||a.kind==MenuAction::TextAccepted){
  if(screen.compare(0,4,"room")==0)RoomAction(a,v,submit);
  else if(a.id=="invite-text")std::snprintf(invitation_,sizeof(invitation_),"%s",a.text.c_str());
  else{
   auto prior=preferences_;
   if(a.id=="name")preferences_.displayName=a.text;else if(a.id=="room-name")preferences_.roomName=a.text;
   else if(a.id=="capacity")preferences_.roomCapacity=(std::max)(2,(std::min)(16,preferences_.roomCapacity+a.delta));
   else if(a.id=="delay")preferences_.inputDelay=(std::max)(0,(std::min)(10,preferences_.inputDelay+a.delta));
   else if(a.id=="hud-size")preferences_.matchHudSize=(std::max)(0,(std::min)(2,preferences_.matchHudSize+a.delta));
   else if(a.id=="hud-spacing")preferences_.matchHudRaised=a.delta>0;
   else if(a.id=="scale")preferences_.interfaceScale=(std::max)(1.f,(std::min)(1.5f,preferences_.interfaceScale+.05f*a.delta));
   else if(a.id=="hud")preferences_.showMatchHud=a.delta>0;else if(a.id=="presence")preferences_.discordPresence=a.delta>0;
   else if(a.id=="invites")preferences_.discordInvites=a.delta>0;else AdjustRule(preferences_.tableRules,a);
   if(!preferences_.Valid()){preferences_=prior;error_="That value is invalid. Your saved setting is unchanged.";}
   else{preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;error_.clear();}
  }
 }
 ImGui::End();ImGui::PopStyleVar(2);
}
} }
