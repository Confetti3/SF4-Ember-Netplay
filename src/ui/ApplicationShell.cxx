#include "ApplicationShell.hxx"
#include <imgui_internal.h>
#include "ButtonPromptSetting.hxx"
#include "ControllersScreen.hxx"
#include "Theme.hxx"
#include "MenuRows.hxx"
#include "RowId.hxx"
#include "RoomFeedback.hxx"
#include "NetworkFeedback.hxx"
#include "MenuPresentation.hxx"
#include "SignInSites.hxx"
#include "RoomControls.hxx"
#include "V2.hxx"
#include "CountryFlag.hxx"
#include "WatchedPicture.hxx"
#include "VersionLine.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../common/NativeMenuLock.hxx"
#include "../common/ReplayInputs.hxx"
#include "../platform/LocaleWindows.hxx"
#include "ReplaySummaryText.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include <imgui.h>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
namespace sf4e { namespace ui {
namespace {
constexpr double ErrorSeconds = 5;
// The launch card names only the settings that differ, in the order the game's
// own Options menu lists them. Empty means nothing to say.
std::string GameSettingsAdvice(const gameconfig::DisplaySettings& g) {
    const struct { bool ok; const char* id; const std::string& value; } items[] = {
        {g.FrameRateOk(), "game_settings.frame_rate", g.frameRate},
        {g.MsaaOk(), "game_settings.anti_aliasing", g.msaa},
    };
    std::string text;
    for (const auto& item : items) {
        if (item.ok) continue;
        if (text.empty()) text = loc::T("game_settings.intro");
        text += "\n\n" + loc::Tf(item.id, item.value);
    }
    return text;
}
// What the session is reporting, in words: the text a caller supplied, else the
// sentence for the condition the controller observed. A recovery that has
// offered a replacement says so instead of the condition that led to it.
std::string SessionProblem(const netplay::Snapshot& session) {
    if(session.recovery==netplay::Recovery::ReplacementOffered&&session.fault!=netplay::Fault::None)return loc::T("room.control_unavailable");
    if(!session.error.empty())return session.error;
    switch(session.fault){
    case netplay::Fault::ControlRecovering:return loc::T("room.control_recovering");
    case netplay::Fault::CatchingUp:return loc::T("room.catching_up");
    default:return {};
    }
}
// The room's own status line: the kind of room it is, and whether it is locked.
const char* RoomStatusKey(const room::Snapshot& room) {
    if(room.serverOwned)return room.locked?"room.public_locked_status":"room.public_status";
    if(room.practice)return room.locked?"room.practice_locked_status":"room.practice_status";
    if(room.arcade)return room.locked?"room.arcade_locked_status":"room.arcade_status";
    return room.locked?"room.locked_status":"room.private_status";
}
}
float ApplicationShell::PublicListWidth() {
    const float available=ImGui::GetContentRegionAvail().x;
    return available>=820*Scale()?available*.53f:available;
}
std::string TrainingReadyText(int seconds) { return loc::Tf("room.training_call.ready_in",seconds); }
bool ApplicationShell::Service(platform::ServiceAction kind, const ShellView& view, const Submit& submit) {
    ShellAction action; action.service = kind; action.command.generation = view.session.generation;
    if (!submit(std::move(action))) { error_ = loc::T("error.queue_failed"); return false; }
    error_.clear(); return true;
}
void ApplicationShell::Refuse(std::string text, std::function<bool(const ShellView&)> stillBlocked) {
    error_ = std::move(text); errorBlockedText_ = error_; errorBlocked_ = std::move(stillBlocked);
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
        action.preferences.lobby.editionSelect = action.preferences.tableRules.editionSelect;
        action.preferences.lobby.roundCount = action.preferences.tableRules.roundCount;
        action.preferences.lobby.roundTime = action.preferences.tableRules.roundTime;
    }
    action.practiceRoom = kind == netplay::CommandKind::HostRoom && createPractice_;
    action.arcadeRoom = kind == netplay::CommandKind::HostRoom ? createArcade_ : room::ClassicRoom;
    if (kind == netplay::CommandKind::HostRoom) action.delayPolicy = preferences_.roomDelayPolicy;
    // Opened from a template: the runtime sets its tables once the room is up.
    if (kind == netplay::CommandKind::HostRoom && createTables_) action.createdRules = CreateTables(preferences_.tableRules);
    if (kind == netplay::CommandKind::JoinInvite) action.command.invitation = invitation_;
    // A room opened here is not the public one the panel joined.
    if (kind == netplay::CommandKind::JoinInvite || kind == netplay::CommandKind::HostRoom) publicRooms_.ForgetCurrent();
    if (!submit(std::move(action))) { error_ = loc::T("error.queue_failed"); return false; }
    error_.clear();
    if (kind == netplay::CommandKind::JoinInvite || kind == netplay::CommandKind::LeaveRoom)
        std::fill(std::begin(invitation_), std::end(invitation_), '\0');
    return true;
}


// Parts of Draw, in the order it runs them.
void ApplicationShell::UpdateRoomTransitions(const ShellView& v,double now) {
 using namespace netplay; auto& nav=menu_.navigation;
 if(previousRoomState_!=RoomState::Idle && v.session.room==RoomState::Idle) {
  nav.Cancel();
  if(nav.Screen().compare(0,4,"room")==0 || nav.Screen()=="selection")nav.Home();
  error_.clear();
  Say(previousRoomState_==RoomState::Opening?"":loc::T("notice.left_room"),Tone::Neutral,3,ToastKind::Room);
 }
 // The room screen opens only once the committed room snapshot has the local
 // member in it. Before that the player stays where they pressed Create or
 // Join, with a pending status, instead of seeing a bare placeholder list.
 if(v.session.room==RoomState::Joined&&previousRoomState_!=RoomState::Joined&&nav.Screen().compare(0,4,"room")!=0){nav.Home();nav.Push("room");}
 previousRoomState_=v.session.room;
 // Opened over a fight: the board, at End practice over a Practice match this
 // PC fights, at Stop watching over a game it only watches.
 if(hidden_&&(v.practiceFight||v.watchedGame.Active())){nav.Home();nav.Push("room");nav.Prefer(v.practiceFight?"end-practice":"stop-watching");}
 hidden_=false;
  if(!(generation_==v.session.generation)) {
  const bool roomChanged=generation_.room!=v.session.generation.room;
  generation_=v.session.generation; nav.Cancel(); error_.clear();notice_.clear();
  // A replacement room that is already joined lands on its own room screen.
  if(roomChanged&&v.session.room==RoomState::Joined&&nav.Screen()!="room"){nav.Home();nav.Push("room");}
  if(v.session.room==RoomState::Idle&&nav.Screen().compare(0,4,"room")==0)nav.Home();
  if(roomChanged){roomUpdateUntil_=0;roomUpdateStarted_=-1;roomDetails_.clear();}
 }
}
bool ApplicationShell::UpdateRoomFeedback(const ShellView& v) {
 using namespace netplay;
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
 return healthyRoom;
}
void ApplicationShell::UpdatePreferenceSave(const ShellView& v,const Submit& submit) {
 using namespace netplay; auto& nav=menu_.navigation;
 if(saveQueued_&&!v.settingsPending){
  if(SamePreferences(v.preferences,savingPreferences_)&&v.settingsError.empty()){
   saveQueued_=false;retrySave_=false;preferencesDirty_=!SamePreferences(preferences_,savingPreferences_);
   if(profileSavePending_&&!preferencesDirty_){
    profileSavePending_=false;error_.clear();ShowNotice(loc::Tf("notice.profile_saved",selection::FindFighter(v.preferences.mainFighter)->name),Tone::Success,ImGui::GetTime()+3);
    if(nav.Screen()=="main-character")nav.Return();
   }
  }else if(ImGui::GetTime()>saveAt_+2){saveQueued_=false;saveFailed_=true;retrySave_=false;error_=loc::T("error.settings_not_saved");}
 }
 if(!preferencesDirty_&&!saveQueued_&&!saveFailed_&&!v.settingsPending)preferences_=v.preferences;
 SyncLayoutPaths(v);
 if(!v.settingsError.empty()&&!retrySave_&&!saveQueued_)saveFailed_=true;
 if(preferencesDirty_&&!saveQueued_&&!saveFailed_&&!v.settingsPending&&(v.canEditPreferences||(v.atMainMenu&&v.session.room!=RoomState::Idle&&!netplay::Fighting(v.session.match)&&OnlyRoomSettingsChanged(v.preferences,preferences_)))&&preferences_.Valid()&&ImGui::GetTime()>=saveAt_){
  if(SavePreferences(v,submit)){saveQueued_=true;savingPreferences_=preferences_;saveAt_=ImGui::GetTime();}else saveFailed_=true;
 }
}
std::vector<MenuEntry> ApplicationShell::BuildRows(const ShellView& v,const std::string& screen,bool idle,bool opening,const DrawSelection& selection,const DrawSelection& developer,std::string& title) {
 using namespace netplay; auto& nav=menu_.navigation;
 std::vector<MenuEntry> rows;
 if(v.inputCapture!=input::Capture::Idle){
  title=loc::T("controller.assign_title");
  rows={Row("capture-cancel",loc::T("controller.cancel_assignment"),std::string(v.inputCapture==input::Capture::Press?loc::T("controller.press_button"):loc::T("controller.release_buttons"))+"\n"+loc::T("controller.keep_current"))};
  rows[0].hint=loc::T("common.cancel");
  rows[0].detailText=DetailText::Name;
 }else if(const ScreenPanel* owner=ScreenOf(screen)){
  // The panels' screens build their own rows (ScreenOf).
  rows=owner->Rows(ScreenFrame{*this,v,screen},idle,title);
  // A match link's row is focused once the matches screen lists it.
  if(screen=="tournament-matches"){const auto focus=identity_.TakeFocus(rows);if(!focus.empty())nav.Focus(focus,rows);}
 }else if(screen=="home"){
  rows={Row("online",loc::T("home.online"),idle?loc::T("home.online_detail"):loc::T("home.return_room")),
   Row("selection",loc::T("home.fighter_select"),v.canEditSelection?v.selectionSummary:
    v.selectionLockReason.empty()?loc::T("home.selection_locked"):v.selectionLockReason,bool(selection)),
   Row("profile",loc::T("home.profile"),loc::T("home.profile_detail")),
   Row("settings",loc::T("home.settings"),loc::T("home.settings_detail")),
   Row("offline",loc::T("home.offline"),loc::T("home.offline_detail"),idle),
   Row("replays",loc::T("home.replays"),loc::T("home.replays_detail"))};
  // Training straight from Home, before Play offline.
  rows.insert(std::find_if(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="offline";}),Row("training",loc::T("home.training"),loc::T("home.training_detail"),idle));
  // News, the channel's newest releases (the panel under the player card shows the first three).
  if(!v.services.update.news.empty())rows.push_back(Row("news",loc::T("home.news"),v.services.update.news.front().title));
  // Notifications, what Ember said lately (the toasts).
  rows.push_back(Row("notifications",loc::T("home.notifications"),toasts_.empty()?std::string(loc::T("notifications.none")):toasts_.front().text));
  if(opening)rows[0].detail=OpeningCreates(v)?loc::T("room.creating_status"):loc::T("room.joining_status");
  if(!v.controllerReady)rows.insert(rows.begin(),Row("player",loc::T("home.choose_controller"),loc::T("home.choose_controller_detail")));
  // Ember ID lives under Online play; tournament matches ready to play are
  // still shown on Home, which leads to them, while no room is joined (a
  // match is played from its own room). The count is in the detail: a
  // value beside the label leaves it too little room in longer languages.
  if(const int ready=idle?identity_.ReadyMatches(v):0)
   rows.insert(rows.begin(),Row("home-matches",loc::T("screen.tournament_matches"),loc::Tf("home.identity_matches",ready)));
  // Back leaves a pending invitation's screen without answering it, so Home
  // keeps a way back to it until it is answered or expires.
  if(v.discordPending)rows.insert(rows.begin(),Row("discord-invitation",loc::T("discord.invitation_pending"),loc::T("discord.invitation_pending_detail")));
  // The player's card is a row too (drawn as the card); Select opens the awards wall.
  rows.push_back(Row("awards",loc::T("awards.card"),loc::T("awards.card_detail")));
  if(developer)rows.push_back(Row("developer","Developer","Development tools."));
  // Right on the list reaches the card beside it, Left goes back to the list's top.
  for(auto& r:rows)if(r.id=="awards")r.left=rows.front().id;else r.right="awards";
 }else if(screen=="training"){
  rows=TrainingSelectRows(v,title);
 }else if(screen=="discord-invitation"){
  title=loc::T("discord.invitation_title");rows={Row("invite-cancel",loc::T("discord.cancel_invitation"),loc::T("discord.cancel_invitation_detail"))};
  if(v.discordConfirm)rows.push_back(ConfirmRow("invite-switch",loc::T("discord.switch_room"),v.discordCanSwitch?loc::T("discord.switch_room_detail"):loc::T("discord.wait_game"),v.discordCanSwitch));
  else rows.push_back(Row("invite-wait",loc::T("discord.invitation_pending"),loc::T("discord.invitation_pending_detail"),false));
 }else if(screen=="developer"&&developer){
  // The inspector's selectors are not the shell's own: they inherit no room
  // hints or Back label, and whatever they forward has no reader here.
  SetEmbeddedReturn({MenuScreenLabel(nav.Parent()),{},false});
  developer();TakeForwardedMenuAction();
  if(ImGui::Button("Back to Home"))nav.Return();
 }else{
  // Any other screen shows Help & about, as it always has.
  static const std::string about="about";
  rows=SettingsOwner().Rows(ScreenFrame{*this,v,about},idle,title);
 }
 return rows;
}
std::pair<std::string,Tone> ApplicationShell::UpdateStatus(const ShellView& v,const std::string& screen,bool opening,bool healthyRoom,std::string& title) {
 using namespace netplay;
 const ScreenPanel* owner=ScreenOf(screen);
 const bool personal=owner&&owner->ReportsSave(screen);
 std::string status=saveFailed_?loc::T("common.save_failed"):v.settingsPending||preferencesDirty_||saveQueued_||languageDirty_?loc::T("common.saving"):personal?loc::T("common.saved"):"";
 // Severity travels with the status string. This line is the shell's only
 // feedback channel, so a failure must not render like ordinary text.
 Tone statusTone=saveFailed_?Tone::Error:v.settingsPending||preferencesDirty_||saveQueued_||languageDirty_?Tone::Pending:
  personal?Tone::Success:Tone::Neutral;
 if((screen=="create"||screen=="join"||screen=="home"||screen=="online"||screen=="public-rooms")&&opening&&status.empty()){
  status=OpeningCreates(v)?loc::T("room.creating_status"):loc::T("room.joining_status");statusTone=Tone::Pending;
 }
 // Chat keeps the board's line, so a lost connection shows while typing.
 if(screen=="room"&&status.empty()){
  const auto place=room::PlaceOf(v.room,v.room.localMember);
  if(place.kind==room::Place::Kind::Seat){
   const auto deadline=room_controls::DescribeReadyDeadline(v,v.room.tables[place.table]);
   if(deadline.seat==place.seat){status=deadline.text;statusTone=Tone::Pending;}
  }
 }
 if((screen=="room"||screen=="room-chat")&&status.empty()){
  const bool healthy=v.session.control==Health::Healthy;
  status=!healthy?std::string(loc::T("room.reconnecting")):v.room.tournament.Active()?
   TournamentRoomStatus(v.room):
   std::string(loc::T(RoomStatusKey(v.room)));
  if(!healthy)statusTone=Tone::Pending;
 }
 if(screen=="room-table"){
  title=loc::Tf("room.table_setup_title",selectedTable_+1);
  const auto& table=v.room.tables[selectedTable_];
  const bool seatedLocal=table.p1==v.room.localMember||table.p2==v.room.localMember;
  // A seated fighter's finished game is background bookkeeping: its stale
  // ready flags and pending result are not the next match's readiness.
  const bool finishedGame=seatedLocal&&table.phase==room::TablePhase::Playing&&v.session.match==MatchState::PostMatch;
  const auto state=[&](room::MemberId id,int side){
   if(!id)return std::string(loc::T("room.waiting_opponent"));
   const auto member=std::find_if(v.room.members.begin(),v.room.members.end(),[&](const room::Member& m){return m.id==id;});
   const bool local=id==v.room.localMember;
   if(member!=v.room.members.end())NoteUserText(member->name);
   return (local?std::string(loc::T("room.you")):member==v.room.members.end()?std::string(loc::T("room.player")):member->name)+
    (table.ready[side]&&!finishedGame?loc::T("room.ready_suffix"):local&&v.readyPress!=netplay::ReadyPress::None?loc::T("room.readying_suffix"):loc::T("room.not_ready_suffix"));
  };
  status=state(table.p1,0)+" | "+state(table.p2,1);
  // Table phases carry their own tone: an unresolved result is a problem
  // the player must act on, a pending result or preparation is a wait.
  if(table.phase==room::TablePhase::Paused){status=loc::T("room.result_unresolved_status");statusTone=Tone::Error;}
  else if(table.phase==room::TablePhase::Ready){
   // A held start counts down on one line; the rows below say who it waits for.
   status=table.spectatorHold?room_controls::HoldStatus(v,table):std::string(loc::T("room.preparing_status"));statusTone=Tone::Pending;
  }
  else if(table.phase==room::TablePhase::Playing&&!finishedGame){
   status=table.resultPending?loc::T("room.waiting_results_status"):loc::T("room.match_in_progress");
   statusTone=Tone::Pending;
  }
  else if(table.phase==room::TablePhase::Closed)status=loc::T("room.table_closed");
  const auto deadline=room_controls::DescribeReadyDeadline(v,table);
  if(seatedLocal&&deadline.seconds&&(deadline.seat?table.p2:table.p1)==v.room.localMember){
   status=deadline.text;statusTone=Tone::Pending;
  }
 }
 // A tournament room says what its next game waits for: the other fighter,
 // or the tournament service's go-ahead.
 if((screen=="room"||screen=="room-table")&&v.room.tournament.Active()){
  if(v.tournament.waitingForPermit){status=loc::T("tournament.state.waiting_permit");statusTone=Tone::Pending;}
  else if(v.tournament.waitingForOpponent){status=loc::T("tournament.state.waiting_opponent");statusTone=Tone::Pending;}
 }
 if(ImGui::GetTime()>=noticeUntil_)notice_.clear();
 if(error_!=lastError_){lastError_=error_;errorSince_=ImGui::GetTime();errorScreen_=screen;}
 // A refusal belongs to the screen it was raised on and to the condition it
 // named; otherwise it lapses on its own.
 if(!error_.empty()&&(screen!=errorScreen_||ImGui::GetTime()-errorSince_>=ErrorSeconds||
   (errorBlocked_&&error_==errorBlockedText_&&!errorBlocked_(v)))){error_.clear();lastError_.clear();}
 // A notice outranks routine save feedback: "Invitation copied." must not
 // vanish because a preference write happens to be in flight. Save failures
 // still win below.
 if(!notice_.empty()&&!saveFailed_){status=notice_;statusTone=noticeTone_;}
 if(!v.padHint.empty()&&statusTone!=Tone::Error){status=v.padHint;statusTone=Tone::Neutral;}
 // The failed language save is about the row on the Accessibility tab (Text
 // and language), so it speaks there only, and every other report below outranks it.
 if(!languageSaveError_.empty()&&SettingsTabOf(screen)=="tab-access"){status=languageSaveError_;statusTone=Tone::Error;}
 // The panel that draws the screen has its say here, under every report below.
 if(owner)owner->Status(ScreenFrame{*this,v,screen},status,statusTone);
 if(v.controllerUnavailable){status=loc::T(v.session.room==RoomState::Idle?"controller.disconnected":"controller.disconnected_room");statusTone=Tone::Error;}
 if(v.session.room==RoomState::Opening&&v.session.openingStalled){status=loc::T("room.opening_stalled");statusTone=Tone::Error;}
 const std::string sessionProblem=SessionProblem(v.session);
 if(!sessionProblem.empty()){status=sessionProblem;statusTone=Tone::Error;}
 if(!v.error.empty()){status=v.error;statusTone=Tone::Error;}
 if(!error_.empty()){status=error_;statusTone=Tone::Error;}
 const bool roomScreen=screen.compare(0,4,"room")==0;
 if(roomScreen&&v.session.recovery!=Recovery::None){
  status=!sessionProblem.empty()?sessionProblem:
   v.session.recovery==Recovery::ReplacementOffered?loc::T("room.control_unavailable"):loc::T("room.control_recovering");
  statusTone=sessionProblem.empty()?Tone::Pending:Tone::Error;
 }
 const auto& selectedTable=v.room.tables[selectedTable_];const auto tablePhase=selectedTable.phase;
 const bool committedMatchStatus=screen=="room-table" && healthyRoom &&
  (tablePhase==room::TablePhase::Ready || tablePhase==room::TablePhase::Playing || tablePhase==room::TablePhase::Paused);
 // A seated fighter's table never swaps its seat line for checkpoint chatter;
 // the runtime carries a Ready press through those gaps on its own.
 const bool seatedTableStatus=screen=="room-table" && (selectedTable.p1==v.room.localMember||selectedTable.p2==v.room.localMember);
 if(roomScreen && (v.session.room==RoomState::Closing ||
    (v.session.control==Health::Healthy && v.session.recovery==Recovery::None &&
     ((!RoomActionsAvailable(v)&&!RoomCheckpointPending(v))||(roomUpdateVisible_&&!seatedTableStatus)) && !committedMatchStatus &&
     !v.controllerUnavailable&&sessionProblem.empty()&&v.error.empty()&&error_.empty())))
  {status=RoomWaitReason(v);statusTone=Tone::Pending;}
 return {status,statusTone};
}
void ApplicationShell::PublishPlayerCard(const ShellView& v) {
 using namespace netplay;
 PlayerCardView card;card.name=preferences_.displayName;
 card.country=ShownCountry(preferences_.country,v.automaticCountry);
 // The profile card (the L tag): Home's and the Player tab's.
 auto& tag=card.profile;const auto& standing=v.rankings.standing;const auto& record=v.preferences.record;
 tag.name=card.name;tag.marks.country=card.country;tag.fighter=preferences_.mainFighter;
 tag.title=v.playerTitle;tag.tier=v.playerTier;tag.icon=v.playerIcon;
 // What the player picked on the awards wall, while it is earned.
 if(const auto* a=WornAward(v,preferences_.wornTitle)){tag.title=loc::T(a->title);tag.tier=a->tier;}
 if(const auto* a=WornAward(v,preferences_.wornIcon))tag.icon=a->icon;
 if(standing.loaded&&standing.value.kind==netplay::rankings::Standing::Kind::Ranked&&standing.value.classesShown)tag.marks.badge=standing.value.badge;
 if(card.country.Present())tag.about=CountryName(card.country);
 if(tag.marks.badge.HasClass())tag.about+=(tag.about.empty()?"":" \xC2\xB7 ")+loc::Tf("class.tip",ClassName(tag.marks.badge.band));
 tag.facts={{loc::T("profile.main_badge"),selection::FindFighter(preferences_.mainFighter)->name}};
 if(record.available)tag.facts.push_back({loc::T("profile.record"),loc::Tf("profile.record_value",record.wins,record.losses)});
 SetMenuPlayerCard(std::move(card));
 std::vector<MenuNewsItem> news;
 for(const auto& n:v.services.update.news)news.push_back({n.title,n.date.empty()?n.tag:n.date+" \xC2\xB7 "+n.tag});
 SetMenuNews(std::move(news));
}
void ApplicationShell::HandleActivate(const MenuAction& a,const ShellView& v,const std::string& screen,bool idle,const Submit& submit) {
 using namespace netplay; auto& nav=menu_.navigation;
 // The panel that owns the screen takes its own rows; a row it does not take
 // goes on to the shell's below.
 if(const ScreenPanel* owner=ScreenOf(screen))if(owner->Activate(ScreenFrame{*this,v,screen},a,submit))return;
 if(ControllerActivate(a,v,submit))return;
 // An opening room keeps its own screen, with its Stop row, until it joins.
 if(a.id=="online")nav.Push(idle?"online":v.session.room==RoomState::Opening?OpeningScreen(v):"room");
 else if(a.id=="discord-invitation")nav.Push(a.id);
 else if(a.id=="tournament-account")identity_.OpenTournamentAccount(v,nav);
 // The matches screen, by way of Online play and Ember ID so Back goes there.
 else if(a.id=="home-matches"){nav.Push("online");nav.Push("identity");nav.Push("tournament-matches");}
 else if(a.id=="profile"||a.id=="main-character")nav.Push(a.id);
 else if(a.id=="awards")nav.Push("awards");
 else if(a.id=="country"){countryPicker_.Open();nav.Push(CountryPicker::Screen);nav.Prefer(CountryPicker::RowId(preferences_.country));}
 // A letter on the board goes to its first country, whose code it carries.
 else if(screen==CountryPicker::Screen&&a.id.compare(0,7,"letter-")==0)nav.Prefer("country-"+a.id.substr(7));
 // A choice is saved like any setting, and the picker closes on it.
 else if(const auto choice=screen==CountryPicker::Screen?countryPicker_.Choose(a.id):std::nullopt){if(v.canEditPreferences){preferences_.country=*choice;preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;error_.clear();nav.Return();}}
 else if(const auto main=RowKey<int>(a.id,"main-");main&&v.canEditPreferences){preferences_.mainFighter=*main;preferencesDirty_=true;profileSavePending_=true;error_.clear();saveAt_=ImGui::GetTime()+.45;}
 else if(screen=="training"&&a.id.compare(0,9,"training-")==0)TrainingSelectActivate(a,v,submit);
 else if(a.id=="selection"){selectionFresh_=true;selectionOpenOn_=screen.compare(0,4,"room")==0?"roster":"";nav.Push(a.id);}
 else if(a.id=="replays"||a.id=="controllers"||a.id=="news"||a.id=="notifications")nav.Push(a.id);
 // News' Update now, confirmed on its row: the launch check's offer, as its question's Update now sends it.
 else if(a.id=="news-update-now"&&v.services.launchOffer){
  ShellAction r;r.command.generation=v.session.generation;r.service=platform::ServiceAction::UpdateNow;r.updateOffer=*v.services.launchOffer;
  if(!submit(std::move(r)))error_=loc::T("error.queue_failed");else updatePrompt_.Settle(*v.services.launchOffer);
 }
 else if(a.id=="replay-folder")Service(platform::ServiceAction::OpenReplayFolder,v,submit);
 else if(a.id=="create"||a.id=="pr-create"||a.id=="pr-none"||a.id=="host"||a.id=="delete-template")CreateActivate(a,v,submit);
 else if(a.id=="settings")nav.Push("tab-player");
 else if(SettingsTab(a.id))nav.Push(a.id);
 else if(a.id=="join"||a.id=="public-rooms"||a.id=="player"||a.id=="identity"||a.id=="rankings"||a.id=="developer"||a.id=="sent-reports")nav.Push(a.id);
 else if(a.id=="join-now")Send(CommandKind::JoinInvite,v,submit);
 else if(a.id=="cancel-open")Send(CommandKind::LeaveRoom,v,submit);
 else if(a.id=="offline"||a.id=="game-buttons")Send(CommandKind::StartOffline,v,submit);
 // Training: the game's main menu goes straight to Training mode, if the runtime finds no room or match.
 else if(a.id=="training"){ShellAction training;training.command.generation=v.session.generation;training.training=TrainingEntry::Home;
  if(!submit(std::move(training)))error_=loc::T("home.training_unavailable");}
 else if(a.id=="paste"){const char* t=ImGui::GetClipboardText();if(t&&*t&&std::strlen(t)<sizeof(invitation_)){std::strcpy(invitation_,t);error_.clear();}else error_=loc::T("error.invitation_invalid");}
 else if(a.id=="capture"||a.id=="keyboard"){ShellAction r;r.command.generation=v.session.generation;r.inputAction=a.id=="capture"?input::Action::BeginCapture:input::Action::UseKeyboard;submit(std::move(r));}
 else if(a.id=="invite-cancel"||a.id=="invite-switch"){ShellAction r;r.command.generation=v.session.generation;r.discordRevision=v.discordRevision;r.discordAction=a.id=="invite-cancel"?discord::InviteAction::Cancel:discord::InviteAction::Switch;if(!submit(std::move(r)))error_=loc::T("error.invitation_changed");}
 else if(a.id=="retry-save"){saveFailed_=false;retrySave_=true;preferencesDirty_=true;saveAt_=0;error_.clear();}
 else if(a.id=="diagnostics")Service(platform::ServiceAction::ExportDiagnostics,v,submit);
 else if(a.id=="problem-report"){
  if(Service(platform::ServiceAction::PrepareProblemReport,v,submit)){reportPanel_.Open(v.services.reporting.preparation + 1);menu_.navigation.Push("report");}
 }
 else if(a.id=="arrange-reset"){preferences_.roomColumn=netplay::DefaultRoomColumn;preferences_.roomSplit=netplay::DefaultRoomSplit;preferences_.roomShowMembers=preferences_.roomShowChat=true;preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;}
 else if(a.id=="ready-test"){
  // The volume on screen, which may not be saved yet.
  ShellAction r;r.command.generation=v.session.generation;r.previewSoundVolume=preferences_.readySoundVolume;
  if(!submit(std::move(r)))error_=loc::T("error.queue_failed");
 }
 else if(a.id=="channel"){
  // Confirmed: the pick is saved and checked; the row shows it until that ends.
  if(!channelPick_.Pending(v.services.channel))return;
  ShellAction r;r.service=platform::ServiceAction::SwitchUpdateChannel;r.updateChannel=*channelPick_.picked;r.command.generation=v.session.generation;
  channelPick_.applying=submit(std::move(r));
  if(!channelPick_.applying)error_=loc::T("error.queue_failed");
 }
 else if(ActivateSpeech(a)){}
 else if(a.id=="updates")Service(platform::ServiceAction::CheckUpdates,v,submit);
 else if(a.id=="update-now"){
  const auto& offer=v.services.offer;
  if(!offer)return;
  ShellAction r;r.service=platform::ServiceAction::UpdateNow;r.updateOffer=*offer;r.command.generation=v.session.generation;
  if(!submit(std::move(r)))error_=loc::T("error.queue_failed");
 }
 else if(a.id=="recovery")Service(platform::ServiceAction::OpenRecovery,v,submit);
 else if(a.id=="community")Service(platform::ServiceAction::OpenCommunity,v,submit);
 else if(screen.compare(0,4,"room")==0)RoomAction(a,v,submit);
}
void ApplicationShell::HandleAdjust(const MenuAction& a,const ShellView& v,const std::string& screen,const Submit& submit) {
 using namespace netplay;
 const ScreenPanel* owner=ScreenOf(screen);
 // The room's screens first, then the panel that owns the screen; what neither takes is the shell's.
 if(screen.compare(0,4,"room")==0)RoomAction(a,v,submit);
 else if(owner&&owner->Adjust(ScreenFrame{*this,v,screen},a)){}
 else if(a.id=="invite-text")std::snprintf(invitation_,sizeof(invitation_),"%s",a.text.c_str());
 else if(a.id=="language")SetLanguage(std::string(loc::NextPreference(languagePreference_,a.delta)));
 else if(a.id=="launch-update-check")SetLaunchCheck(a.delta>0);
 else if(a.id=="channel"){if(!v.services.pending)channelPick_.Step(v.services.channel,a.delta);}
 else if(a.id=="country-search")countryPicker_.Search(a.text);
 else if(CreateAdjust(a)){}
 else{
  auto prior=preferences_;
  if(a.id=="name")preferences_.displayName=a.text;else if(a.id=="room-name")preferences_.roomName=a.text;
  else if(a.id=="capacity")preferences_.roomCapacity=(std::max)(2,(std::min)(16,preferences_.roomCapacity+a.delta));
  else if(a.id=="delay"){
   // Auto sits before the smallest delay. Choosing it keeps the number; leaving it starts at the smallest.
   const int next=StepInputDelay(preferences_.autoInputDelay,preferences_.inputDelay,a.delta);
   preferences_.autoInputDelay=next==AutoInputDelayChoice;if(!preferences_.autoInputDelay)preferences_.inputDelay=next;
  }
  else if(a.id=="auto-style")preferences_.autoDelayStyle=(std::max)(0,(std::min)(AutoDelayStyles-1,preferences_.autoDelayStyle+a.delta));
  else if(AdjustSetting(preferences_,a)||AdjustOnOff(preferences_,a.id,a.delta)||AdjustSpeech(a)){}
  else if(a.id=="button-prompts")Step(preferences_.buttonPrompts,{0,1,2,3},a.delta);
  else if(a.id=="visibility"){preferences_.roomPublic=a.delta>0;if(preferences_.roomPublic)ApplyPublicDefaultName();} // UpdatePublicBridge asks the Ember ID for what Public needs
  else if(a.id=="delay-policy")preferences_.roomDelayPolicy=a.delta>0?room::DelayPolicy::Own:room::DelayPolicy::Shared;
  else AdjustRule(screen=="create"&&preferences_.roomPublic?preferences_.publicTableRules:preferences_.tableRules,a);
  if(!preferences_.Valid()){preferences_=prior;error_=loc::T("error.invalid_value");}
  else{preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;error_.clear();}
 }
}

// What a notice's button does once the menu has drawn the frame it closed on.
// The buttons carry no actions of their own, only results (NoticeResult).
void ApplicationShell::AnswerDialog(const Submit& submit) {
 const NoticeResult result=menu_.TakeNoticeResult();
 switch(result){
 case NoticeResult::CreatePublicRoom:openPublicCreate_=true;break;
 case NoticeResult::HideGameSettings:{std::string diagnostic;platform::HideGameSettingsCardForever(diagnostic);break;}
 case NoticeResult::OpenButtonLayout:if(dialogContext_.pad)OpenButtonLayout(*dialogContext_.pad);break;
 case NoticeResult::LeaveButtonLayout:{
  ShellAction log;log.command.generation=dialogContext_.generation;log.padAction.kind=gamepads::ControllerRequest::Kind::LayoutLog;
  log.padAction.log=dialogContext_.log;submit(std::move(log));menu_.navigation.Return();break;
 }
 default:break;
 }
 // Any closed notice answers the update question if it is the one on show.
 if(updatePrompt_.Open()&&!menu_.NoticeOpen())FinishUpdatePrompt(result,submit);
}
// Quick join asks for the first room with a free seat, in the player's region when it is known.
// With none it offers to create one, which the next frame opens.
void ApplicationShell::QuickJoin(const ShellView& v) {
 error_.clear();
 if(const auto id=publicRooms_.QuickPick(v,PublicRoomsPanel::KnownRegion(v))){
  if(publicRooms_.Join(*id))menu_.navigation.Prefer("pr-room:"+*id);
  return;
 }
 menu_.ShowNotice(NoticeDialog{loc::T("public.quick_none"),loc::T("public.quick"),{{loc::T("common.ok")},{loc::T("public.create_room"),NoticeResult::CreatePublicRoom}}});
}

void ApplicationShell::SetLanguage(std::string preference) {
 // Not a netplay preference: it lives in the UI preferences file and saves on
 // its own deadline, so it stays out of the validate-and-save tail of HandleAdjust.
 if(!loc::ValidPreference(preference))return;
 languagePreference_=std::move(preference);
 loc::SetActive(platform::ResolveUiLocale(languagePreference_));
 languageDirty_=true;languageSaveError_.clear();languageSaveAt_=ImGui::GetTime()+.45;
}
void ApplicationShell::Background(const ShellView& v,const room::Snapshot& room,const Submit& submit) {
 CancelLayoutCapture(v,submit);
 if(preferencesDirty_&&OnlyRoomSettingsChanged(v.preferences,preferences_))UpdatePreferenceSave(v,submit);
 UpdatePadSetup(v,submit,false);
 Conceal();hidden_=true;
 UpdatePostMatch(v,room,ImGui::GetTime(),submit,nullptr);
 // Not read: the player is not looking at Chat while Ember is hidden, whatever screen it was left on.
 ObserveChat(v,room);
 identity_.Hidden(v,submit,ImGui::GetTime());
 DrawToasts(ImGui::GetTime(),true);
}

v2::Box ApplicationShell::ShellArea() const {
 // TV mode keeps Ember inside a 5% safe area, which a TV may crop.
 const auto* vp=ImGui::GetMainViewport();const ImVec2 safe=preferences_.tvMode?ImVec2(vp->Size.x*.05f,vp->Size.y*.05f):ImVec2(0,0);
 return {ImVec2(vp->Pos.x+safe.x,vp->Pos.y+safe.y),ImVec2(vp->Pos.x+vp->Size.x-safe.x,vp->Pos.y+vp->Size.y-safe.y)};
}
void ApplicationShell::Draw(const ShellView& v,bool* open,const Submit& submit,const DrawSelection& selection,const DrawSelection& developer) {
 using namespace netplay; auto& nav=menu_.navigation;
 boardDrawn_=false;selectionDrawn_=false;
 const double now = ImGui::GetTime();
 UpdatePadSetup(v,submit,true);
 if(!languageSeeded_){languagePreference_=loc::ValidPreference(v.languagePreference)?v.languagePreference:"auto";languageSeeded_=true;}
 // Called out of Training: the player's own table is shown, where the Ready
 // row carries the time left, and a player who asked for it is readied.
 if(v.trainingCallSequence!=trainingCallSeen_||v.trainingReadySequence!=trainingReadySeen_){
  const auto place=room::PlaceOf(v.room,v.room.localMember);
  const bool seated=v.session.room==RoomState::Joined&&place.kind==room::Place::Kind::Seat&&place.table>=0&&place.table<static_cast<int>(v.room.tables.size());
  if(seated&&v.trainingCallSequence!=trainingCallSeen_)OpenTableOptions(v,place.table);
  trainingCallSeen_=v.trainingCallSequence;
  // The Ready waits for the runtime's gate, and ends with the seat or the window.
  // A refused submit keeps the request, since the runtime asks for the Ready only once.
  if(!seated||v.trainingReadySeconds<=0)trainingReadySeen_=v.trainingReadySequence;
  else if(v.trainingReadySequence!=trainingReadySeen_&&v.canReady){selectedTable_=place.table;if(Send(CommandKind::Ready,v,submit))trainingReadySeen_=v.trainingReadySequence;}
 }
 if(lastUiTime_ >= 0 && now < lastUiTime_) {
  // DX9 reset recreates ImGui, but these deadlines belong to the surviving shell.
  // Keep raw-clock users in RoomAction in the same epoch, including queued saves.
  saveAt_ = RebaseUiTimestamp(saveAt_, lastUiTime_, now);
  languageSaveAt_ = RebaseUiTimestamp(languageSaveAt_, lastUiTime_, now);
  noticeUntil_ = RebaseUiTimestamp(noticeUntil_, lastUiTime_, now);
  shortCopyUntil_ = RebaseUiTimestamp(shortCopyUntil_, lastUiTime_, now);
  roomUpdateUntil_ = RebaseUiTimestamp(roomUpdateUntil_, lastUiTime_, now);
  if(postMatch_)postMatch_->until = RebaseUiTimestamp(postMatch_->until, lastUiTime_, now);
  roomUpdateStarted_ = -1;
  for(auto& t:toasts_){t.at=RebaseUiTimestamp(t.at,lastUiTime_,now);t.until=RebaseUiTimestamp(t.until,lastUiTime_,now);}
 }
 lastUiTime_ = now;
 // Player-written text can need glyphs the atlas has not baked yet. The
 // renderer tells the theme about each string it draws, where it draws it, so
 // text that is scrolled out of view, muted or on another screen holds none.
 if(!gameSettingsChecked_&&v.showGameSettingsCard){
  // Once per launch. "Don't show again" is the card's own outcome (AnswerDialog);
  // a failed save just means the card returns.
  gameSettingsChecked_=true;
  std::string advice=GameSettingsAdvice(v.gameSettings);
  if(!advice.empty())
   menu_.ShowNotice(NoticeDialog{std::move(advice),loc::T("game_settings.title"),{{loc::T("common.ok")},{loc::T("game_settings.hide"),NoticeResult::HideGameSettings}}});
 }
 if(languageDirty_&&now>=languageSaveAt_){
  // The store's detail is an English diagnostic, so the player sees the
  // localized message instead. sf4e_ui has no log to carry the detail to.
  std::string diagnostic;
  if(languageSaver_?languageSaver_(languagePreference_,diagnostic):platform::SaveLanguagePreference(languagePreference_,diagnostic))languageSaveError_.clear();
  else languageSaveError_=loc::T("settings.language_save_failed");
  languageDirty_=false;
 }
 UpdateRoomTransitions(v,now);
 UpdatePostMatch(v,v.room,now,submit,open);
 TrackLiveGames(v,now);
 TrackLockIn(v,now);
 UpdateChat(v,submit);
 const bool healthyRoom=UpdateRoomFeedback(v);
 UpdatePreferenceSave(v,submit);
 UpdateShortCopy(v,now);
 UpdateJoinLink(v,now,submit);
 UpdatePublicRoomLink(v,now);
 if(openPublicCreate_){
  openPublicCreate_=false;
  if(v.session.room==RoomState::Idle&&v.canOpenRoom&&nav.Screen()=="public-rooms")OpenPublicCreate();
 }
 if(v.readyFailureSequence!=readyFailureSequence_){
  readyFailureSequence_=v.readyFailureSequence;
  if(readyFailureSequence_&&!v.readyFailure.empty())menu_.ShowError(v.readyFailure);
 }
 // An invitation is a toast (Join opens its screen), and a row on Home while it waits: it never takes the player off what they were doing.
 if(v.discordPending&&inviteRevision_!=v.discordRevision){inviteRevision_=v.discordRevision;
  Say(loc::T("discord.invitation_notice"),Tone::Pending,30,ToastKind::Invitation,{{loc::T("toast.join"),"discord-invitation","invite-switch"},{loc::T("updates.launch_later")}});}
 if(!v.discordPending&&nav.Screen()=="discord-invitation")nav.Return();
 // A replay link takes the menu to its question only with no room: in a room the menu is the room's, and the link waits until it is left.
 replays_.Update(v,nav);
 if(v.inputCapture!=input::Capture::Idle&&nav.Screen()!="assignment")nav.Push("assignment");
 if(v.inputCapture==input::Capture::Idle&&nav.Screen()=="assignment")nav.Return();
 identity_.Update(v,nav.Screen(),submit,now);
 publicRooms_.Update(v,nav.Screen(),identity_.UsableBridge(v),identity_.Waiting(),submit,now);
 UpdatePublicBridge(v,nav.Screen());
 {std::string said;if(publicRooms_.TakeSaid(said)){ShowNotice(said,Tone::Error,now+6);}}
 rankings_.Update(v,nav.Screen(),identity_.UsableBridge(v),RankingsPanel::OwnListing(preferences_,v.automaticCountry),submit);
 {std::string said;if(rankings_.TakeSaid(said)){ShowNotice(said,Tone::Error,now+6);}}
 // The setup finished while the player waited or looked away: say so once, and the list is already on its way.
 if(identity_.PublicSetup().step==PublicSetupStep::Done){identity_.ClearPublicSetup();ShowNotice(loc::T("public.setup_done"),Tone::Success,now+4);}
 // A tournament match that ended is announced wherever the player is: the
 // room it was played in closes with it.
 if(v.tournament.phase!=tournamentPhase_){
  using netplay::tournament::Phase;
  // One ended for a lost connection says so in the same words on both sides:
  // the tournament is setting up a new match, which Ember does not start itself.
  if(v.tournament.phase==Phase::Finished&&v.tournament.reason==netplay::tournament::ConnectionLostReason){
   Say(loc::T("tournament.connection_lost_notice"),Tone::Pending,30,ToastKind::Match);
  }
  else if(v.tournament.phase==Phase::Finished)Say(loc::T("tournament.done_notice"),Tone::Success,12,ToastKind::Match);
  else if(v.tournament.phase==Phase::Failed){
   Say(loc::Tf("tournament.stopped_notice",IdentityPanel::TournamentFailure(v.tournament.reason)),Tone::Error,20,ToastKind::Match);
  }
  tournamentPhase_=v.tournament.phase;
 }
 // A match being watched says how the room it was in ended, once, wherever the player is.
 if(v.tournament.watch.noticeSequence!=watchNoticeSequence_){
  using netplay::tournament::WatchNotice;
  watchNoticeSequence_=v.tournament.watch.noticeSequence;
  switch(v.tournament.watch.notice){
  case WatchNotice::Finished:Say(loc::T("tournament.watch.finished_notice"),Tone::Success,12,ToastKind::Match);break;
  case WatchNotice::Removed:Say(loc::T("tournament.watch.removed_notice"),Tone::Error,20,ToastKind::Match);break;
  case WatchNotice::RoomClosed:Say(loc::T("tournament.watch.room_closed_notice"),Tone::Pending,12,ToastKind::Match);break;
  default:break;
  }
 }
 // A match link from the browser opens the match and plays it by itself once
 // the list shows it playable (IdentityPanel::OpenMatch). It never leaves a
 // room by itself: in one, it only says so, and the match waits for Play.
 if(v.tournament.link.sequence!=matchLinkSequence_){
  matchLinkSequence_=v.tournament.link.sequence;
  const bool idle=v.session.room==RoomState::Idle;
  identity_.OpenMatch(v.tournament.link.bridge,v.tournament.link.match,v.tournament.link.direct&&IdentityPanel::LinkMayPlay(v),v.tournament.link.sequence);
  if(idle){
   if(nav.Screen()!="tournament-matches"){nav.Home();nav.Push("online");nav.Push("identity");nav.Push("tournament-matches");}
   notice_=loc::T("tournament.link_opened");noticeTone_=Tone::Success;
  }else{notice_=loc::T("tournament.link_waiting");noticeTone_=Tone::Pending;}
  noticeUntil_=now+15;
 }
 // A match newly assigned is announced outside a room, so the player never
 // has to go looking for it.
 if(v.session.room==RoomState::Idle&&identity_.TakeAssigned(v)){
  Say(loc::T("tournament.assigned_notice"),Tone::Success,15,ToastKind::Match,{{loc::T("toast.open"),"tournament-matches",""},{loc::T("updates.launch_later")}});
 }
 // A Discord connect link opens Connect Discord for its service, where the
 // player decides. In a room it waits until the room closes.
 if(v.tournament.connect.sequence!=connectLinkSequence_){
  connectLinkSequence_=v.tournament.connect.sequence;pendingConnect_=v.tournament.connect.bridge;
  // In a room the link waits, and so is no longer fresh when it opens.
  if(v.session.room!=RoomState::Idle)pendingConnectStale_=true;
  if(v.session.room!=RoomState::Idle){ShowNotice(loc::T("connect.link_waiting"),Tone::Pending,now+15);}
 }
 if(!pendingConnect_.empty()&&v.session.room==RoomState::Idle){
  // A link the player just clicked goes on by itself; one that arrived
  // during play or waited for a room asks first.
  const bool fresh=!pendingConnectStale_&&!v.tournament.connect.confirm;
  identity_.OpenDiscord(pendingConnect_,fresh);pendingConnect_.clear();pendingConnectStale_=false;
  if(nav.Screen()!="discord-connect"){nav.Home();nav.Push("online");nav.Push("identity");nav.Push("discord-connect");}
 }
 // After every other source of a notice this frame, so the question never takes a place they are about to.
 UpdateLaunchCheck(v,now,submit);
 const auto* vp=ImGui::GetMainViewport();const v2::Box area=ShellArea();
 ImGui::SetNextWindowPos(area.min);ImGui::SetNextWindowSize(ImVec2(area.W(),area.H()));
 const bool postMatchScreen=nav.Screen()=="post-match";
 if(postMatchScreen){
  // A panel in the middle of the WIN screen, which stays in sight around it.
  const ImVec2 size((std::min)(vp->Size.x-32*Scale(),720*Scale()),(std::min)(vp->Size.y-32*Scale(),420*Scale()));
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x+(vp->Size.x-size.x)/2,vp->Pos.y+(vp->Size.y-size.y)/2));ImGui::SetNextWindowSize(size);
 }
 if(v.SeeThrough())ImGui::SetNextWindowBgAlpha(WatchedMatchWindowAlpha);
 ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(!postMatchScreen?v2::SideMargin(area.W(),Scale()):20*Scale(),16*Scale()));ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
 ImGui::Begin("SF4 Ember Netplay###EmberShell",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs);
 menu_.backInert=false;menu_.ownsBack=false;menu_.backdrop=!v.SeeThrough()&&!postMatchScreen;
 // Each screen sets its header (eyebrow, chips), tabs, bell, detail pane and
 // typing below, every frame; the screens drawn first (Button layout, Pad
 // setup) set their own.
 menu_.eyebrow.clear();menu_.headerChips.clear();menu_.tabs.clear();menu_.tab=-1;menu_.statusLeads.clear();menu_.customDetail=false;menu_.typesText=false;
 if(DrawButtonLayout(v,submit)||DrawPadSetup(v,submit)){ImGui::End();ImGui::PopStyleVar(2);return;}
 // The room board shows a Big picture in its tables' place instead (this frame's word, boardPicture_).
 if(v.watchedPicture&&!boardPicture_&&DrawWatchedPicture(v.watchedPicture,preferences_.pictureSpot)){preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;}
 pictureOnBoard_=boardPicture_;boardPicture_=false;
 // Room shortcuts work from every screen of a joined room, including the
 // embedded fighter selector, which forwards what it does not handle.
 const bool inRoom=v.session.room!=RoomState::Idle&&v.room.roomEpoch;
 SetGameplayLayoutPrompts(v);
 if(nav.Screen()=="selection"&&selection){
  // The selector names where its Back goes and shows the room's shortcuts it hands back.
  SetEmbeddedReturn({MenuScreenLabel(nav.Parent()),inRoom?RoomShortcutHints():std::vector<LegendHint>{},selectionFresh_,selectionOpenOn_});
  selectionFresh_=false;selectionOpenOn_.clear();
  // Only what this frame's selector forwards is read below.
  TakeForwardedMenuAction();
  selection();selectionDrawn_=true;
  const auto forwarded=TakeForwardedMenuAction();
  if(forwarded.kind==MenuAction::Close)nav.Return();
  else if(forwarded.kind==MenuAction::Shortcut&&inRoom)RoomShortcut(forwarded,v);
  ImGui::End();ImGui::PopStyleVar(2);return;
 }
 // A fresh install opens on the welcome, once (ApplicationShell__FirstRun.cxx).
 if(!preferences_.welcomed&&!welcomeOffered_&&v.canEditPreferences&&v.session.room==RoomState::Idle&&nav.Screen()=="home"){welcomeOffered_=true;welcomeStep_=0;nav.Push("welcome");}
 const std::string screen=nav.Screen();std::string title=loc::T("shell.home_title");
 if(screen=="post-match"){DrawPostMatch(v,now,submit,open);ImGui::End();ImGui::PopStyleVar(2);return;}
 if(screen=="report") {
  const auto intent=reportPanel_.Draw(v.services,error_.empty()?v.error:error_);
  if(intent.service!=platform::ServiceAction::None){
   ShellAction action;action.service=intent.service;action.reportSubmission=intent.submission;action.command.generation=v.session.generation;
   if(!submit(std::move(action)))error_=loc::T("error.queue_failed");else error_.clear();
  }
  if(intent.close)nav.Return();
  ImGui::End();ImGui::PopStyleVar(2);return;
 }
 const bool idle=v.session.room==RoomState::Idle, opening=v.session.room==RoomState::Opening;
 std::vector<MenuEntry> rows=BuildRows(v,screen,idle,opening,selection,developer,title);
 if(saveFailed_){
  // The store's own diagnostic is English and goes to the log (the runtime
  // writes it there); the row says it in the player's language.
  const std::string reason=v.settingsError.empty()&&!error_.empty()?error_:std::string(loc::T("error.settings_not_saved"));
  rows.push_back(Row("retry-save",loc::T("settings.retry_save"),reason,v.canEditPreferences&&!v.settingsPending));rows.back().wide=true;
 }
 const auto feedback=UpdateStatus(v,screen,opening,healthyRoom,title);
 const std::string& status=feedback.first;const Tone statusTone=feedback.second;
 const bool roomScreen=screen.compare(0,4,"room")==0;
 // Select on a value opens all its values in a panel; one with no list of values has only its arrows.
 OfferValues(rows);
 // Kick or Make host from a member's popout asks its question on the member's page.
 if(screen=="room-member"&&!memberAsk_.empty()){menu_.navigation.Focus(memberAsk_,rows);menu_.navigation.Choose(rows);memberAsk_.clear();}
 // Public rooms say where they sit, and the player's region.
 if(PublicRoomsPanel::Owns(screen)){menu_.eyebrow=loc::T("online.eyebrow_browse");
  if(const char* region=RelayRegionName(PublicRoomsPanel::KnownRegion(v)))menu_.headerChips.push_back({loc::Tf("public.your_region",region),palette::Ready});}
 PublishPlayerCard(v);
 // Public rooms lead with a toolbar of four cells, in one row on a wide list and two on a narrow one.
 const bool publicToolbar=PublicRoomsPanel::Owns(screen)&&!rows.empty()&&rows[0].id=="pr-quick";
 // The panel that draws the screen paints its rows (public rooms' toolbar and
 // cards, the rankings, Settings' board) and its detail pane.
 const ScreenPanel* owner=ScreenOf(screen);
 const ScreenFrame frame{*this,v,screen,&rows};
 ScreenGrid grid=owner?owner->Grid(frame):ScreenGrid{};
 if(publicToolbar)grid={PublicListWidth()>=600*Scale()?4:2,46};
 menu_.listWidth=0;menu_.paneWidth=PublicRoomsPanel::Owns(screen)?380:0;
 GameMenu::Card portraits=owner?owner->Card(frame,now):GameMenu::Card{};
 GameMenu::Detail profilePreview=owner?owner->Detail(frame):GameMenu::Detail{};
 GameMenu::Body board=owner?owner->Body(frame):GameMenu::Body{};
 // Replays' match and set cards and the focused one's pane, and Export video's preview.
 if((screen=="replays"||screen=="replay-sets"||screen=="replay-export")){
  portraits=[&](const MenuEntry& e,ImVec2 min,ImVec2 max){return replays_.DrawCard(v,e,min,max);};
  profilePreview=[&](const std::string& id){replays_.DrawDetail(v,screen,id,nav,rows);};
  menu_.customDetail=screen=="replay-export"||nav.Focus().compare(0,7,"replay:")==0||nav.Focus().compare(0,4,"set:")==0;
  if(screen!="replay-export")menu_.paneWidth=380;
 }
 if(v.inputCapture!=input::Capture::Idle)profilePreview=[&](const std::string&){DrawCaptureDetail(v.inputCapture,v.pads,v.captureCandidate);};
 // Visual grace cannot grant permission: enabled and all dispatch checks stay live.
 const bool checkpointPending=roomScreen && RoomCheckpointPending(v) && !v.controllerUnavailable &&
  SessionProblem(v.session).empty() && v.error.empty() && error_.empty();
 for(auto& row:rows)row.pending=checkpointPending;
 if((PublicRoomsPanel::Owns(screen)&&publicRooms_.Refreshable(v))||(RankingsPanel::Owns(screen)&&rankings_.Refreshable()))menu_.shortcutHints=ShortcutHints({{MenuInput::Options,loc::T("legend.refresh")}});
 else menu_.shortcutHints.clear();
 menu_.statusLeads=screen!="notifications"?"notifications":"";
 if(RankingsPanel::Owns(screen)){
  // Rankings' two pages, your own and the board, are tabs.
  menu_.tabs={{RankingsPanel::Screen,MenuScreenName(RankingsPanel::Screen)},{RankingsPanel::BoardScreen,MenuScreenName(RankingsPanel::BoardScreen)}};
  menu_.tab=screen==RankingsPanel::Screen?0:1;
 }else if(screen=="replays"||screen=="replay-sets"||screen=="replay-export"){
  // Replays' tabs, and whether matches watched are saved too.
  menu_.tabs=replays_.Tabs(v);
  for(int i=0;i<static_cast<int>(menu_.tabs.size());++i)if(menu_.tabs[i].first==screen){menu_.tab=i;menu_.eyebrow=std::string(loc::T("replays.title"))+" \xC2\xB7 "+menu_.tabs[i].second;}
  menu_.headerChips.push_back({std::string(loc::T("replays.save_watched"))+" \xC2\xB7 "+loc::T(preferences_.recordWatched?"common.on":"common.off"),preferences_.recordWatched?palette::Ready:v2::Colors().muted});
 }
 // Back names where it goes when that is not simply the previous screen:
 // from Home, out of Ember or back to the room; on the board, out of your seat (RoomScreens).
 menu_.exitName.clear();menu_.backHint.clear();
 // Home's corner says which version this is and which channel it follows.
 menu_.footerNote=screen=="home"?VersionLine(InstalledVersion(v),v.services.channel):std::string();
 if(screen=="home"){
  // In a room Back does nothing here (the game's own menu is locked behind
  // Ember), so Home offers no way out and the legend no Back.
  if(opening||input::HomeBackHidesShell(v.session.room)){
   menu_.exitName=opening?MenuScreenLabel(OpeningScreen(v)):loc::T("screen.game");
   menu_.backHint=loc::Tf("menu.return_to",menu_.exitName);
  }else menu_.backInert=true;
 }
 // The screen's panel dresses the menu last, over the defaults above.
 if(owner)owner->Dress(frame,menu_);
 // While a controller is being captured it cannot drive the menu, so the
 // legend shows the keyboard and names the cancel.
 if(v.inputCapture!=input::Capture::Idle){SetMenuGlyphs(input::PadKeyboard,0,0);menu_.backHint=loc::T("common.cancel");}
 // An opening public room's card is information, so the cursor waits on its Stop row.
 if(PublicRoomsPanel::Owns(screen)&&opening&&nav.Focus()=="pr-opening")nav.Focus("cancel-open",rows);
 UpdateSpeech();
 // The newest action toast holds focus from when it came over a free menu
 // (DrawToasts): Select takes its first action, Back its last (Later), and
 // any other press leaves it.
 menu_.toastFocused=toastFocus_!=0;
 auto a=menu_.Draw(title.c_str(),rows,status.c_str(),profilePreview,grid.columns,portraits,board,false,grid.cardHeight,statusTone,screen=="home"||screen=="welcome");
 AnswerDialog(submit);
 if(const unsigned pressed=menu_.toastPressed){
  if(pressed&(MenuInput::Select|MenuInput::Back))toastAsked_={toastFocus_,pressed&MenuInput::Select?0:~std::size_t(0)};
  toastFocus_=0;
 }
 if(v.inputCapture!=input::Capture::Idle&&(a.id=="capture-cancel"||a.kind==MenuAction::Returned||a.kind==MenuAction::Close)){
  ShellAction r;r.command.generation=v.session.generation;r.inputAction=input::Action::Cancel;submit(std::move(r));
 }else if(a.kind==MenuAction::Close&&opening){
  // A room being opened keeps its Stop row in reach.
  nav.Push(OpeningScreen(v));
 }else if(a.kind==MenuAction::Close||a.id=="return"){
  // Back at the root hides Ember, unless a room is joined: the game's menu
  // behind it is locked there, so Ember stays up (F10 can still hide it).
  if(open&&input::HomeBackHidesShell(v.session.room))*open=false;
 }else if(a.kind==MenuAction::Shortcut){
  // A tab names the page it shows.
  // A panel's screen sits on its tab's: leave it first, and the tab too unless one of its panels was picked.
  // The hint bar's bell opens Notifications over the screen shown.
  if(a.id=="notifications")nav.Push(a.id);
  // A tab replaces the screen it is on, whatever is open on it (a reader, a choice).
  else if(!a.id.empty()){nav.Cancel();if(a.id=="replay-export")replays_.ExportFrom(v,nav.Focus());if(SettingsTabOf(screen)!=screen)nav.Return();if(SettingsTabOf(a.id)==a.id)nav.Return();nav.Push(a.id);}
  // In a room its shortcuts work from every screen, before any panel's.
  else if(inRoom)RoomShortcut(a,v);
  else if(owner)owner->Shortcut(frame,a);
 }else if(a.kind==MenuAction::Chosen){
  if(roomScreen)RoomAction(a,v,submit);
  else if(owner&&owner->Choose(frame,a,submit)){}
  else if(a.id=="language")SetLanguage(a.text);
  else if(screen=="controllers")HandleActivate(a,v,screen,idle,submit);
 }else if(a.kind==MenuAction::Back){
  // Back reaches the shell only from a screen that takes it itself (the welcome's ownsBack).
  if(owner)owner->Activate(frame,a,submit);
 }else if(a.kind==MenuAction::Activate){
  HandleActivate(a,v,screen,idle,submit);
 }else if(a.kind==MenuAction::Adjust||a.kind==MenuAction::TextAccepted){
  HandleAdjust(a,v,screen,submit);
 }
 // Accepted text may be a passphrase; the panel kept its own copy.
 if(a.kind==MenuAction::TextAccepted)WipeText(a.text);
 AnswerToast(now);
 ImGui::End();ImGui::PopStyleVar(2);
 DrawToasts(now,false,nav.Screen()=="notifications");
}
} }
