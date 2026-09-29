#include "ApplicationShell.hxx"
#include "Theme.hxx"
#include "MenuRows.hxx"
#include "RoomFeedback.hxx"
#include "NetworkFeedback.hxx"
#include "MenuPresentation.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../platform/LocaleWindows.hxx"
#include "../platform/UiPreferencesStore.hxx"
#include <imgui.h>
#include <cstdio>
#include <cstring>
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
// "Automatic" first, then every locale by its own name.
std::vector<MenuChoice> LanguageChoices() {
    std::vector<MenuChoice> choices;std::string preference="auto";
    do {
        choices.push_back({preference,preference=="auto"?loc::Tf("settings.language.system_with",loc::NativeName(platform::ResolveUiLocale("auto"))):
            std::string(loc::NativeName(loc::ResolveLocale(preference,{},{})))});
        preference=std::string(loc::NextPreference(preference,1));
    } while(preference!="auto"&&choices.size()<64);
    return choices;
}
}
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
        action.preferences.tableRules.format = room::SetFormat::Unlimited;
        action.preferences.tableRules.rotation = room::RotationMode::WinnerStays;
        action.preferences.lobby.editionSelect = action.preferences.tableRules.editionSelect;
        action.preferences.lobby.roundCount = action.preferences.tableRules.roundCount;
        action.preferences.lobby.roundTime = action.preferences.tableRules.roundTime;
    }
    if (kind == netplay::CommandKind::JoinInvite) action.command.invitation = invitation_;
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
  notice_=previousRoomState_==RoomState::Opening?"":loc::T("notice.left_room");
  noticeTone_=Tone::Neutral;noticeUntil_=now+3;
 }
 // The room screen opens only once the committed room snapshot has the local
 // member in it. Before that the player stays where they pressed Create or
 // Join, with a pending status, instead of seeing a bare placeholder list.
 if(v.session.room==RoomState::Joined&&previousRoomState_!=RoomState::Joined&&nav.Screen().compare(0,4,"room")!=0){nav.Home();nav.Push("room");}
 previousRoomState_=v.session.room;
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
    profileSavePending_=false;error_.clear();notice_=loc::Tf("notice.profile_saved",selection::FindFighter(v.preferences.mainFighter)->name);noticeTone_=Tone::Success;noticeUntil_=ImGui::GetTime()+3;
    if(nav.Screen()=="main-character")nav.Return();
   }
  }else if(ImGui::GetTime()>saveAt_+2){saveQueued_=false;saveFailed_=true;retrySave_=false;error_=loc::T("error.settings_not_saved");}
 }
 if(!preferencesDirty_&&!saveQueued_&&!saveFailed_&&!v.settingsPending)preferences_=v.preferences;
 if(!v.settingsError.empty()&&!retrySave_&&!saveQueued_)saveFailed_=true;
 if(preferencesDirty_&&!saveQueued_&&!saveFailed_&&!v.settingsPending&&v.canEditPreferences&&preferences_.Valid()&&ImGui::GetTime()>=saveAt_){
  if(Send(CommandKind::SavePreferences,v,submit)){saveQueued_=true;savingPreferences_=preferences_;saveAt_=ImGui::GetTime();}else saveFailed_=true;
 }
}
std::vector<MenuEntry> ApplicationShell::BuildRows(const ShellView& v,const std::string& screen,bool idle,bool opening,const DrawSelection& selection,const DrawSelection& developer,std::string& title) {
 using namespace netplay; auto& nav=menu_.navigation;
 std::vector<MenuEntry> rows;
 const char* reason=v.canEditPreferences?loc::T("settings.auto_save"):loc::T("settings.leave_room_to_edit");
 if(v.inputCapture!=input::Capture::Idle){
  title=loc::T("controller.assign_title");
  rows={Row("capture-cancel",loc::T("controller.cancel_assignment"),std::string(v.inputCapture==input::Capture::Press?loc::T("controller.press_button"):loc::T("controller.release_buttons"))+"\n"+loc::T("controller.keep_current"))};
  rows[0].hint=loc::T("common.cancel");
 }else if(screen=="home"){
  rows={Row("online",loc::T("home.online"),idle?loc::T("home.online_detail"):loc::T("home.return_room")),
   Row("selection",loc::T("home.fighter_select"),v.canEditSelection?v.selectionSummary:
    v.selectionLockReason.empty()?loc::T("home.selection_locked"):v.selectionLockReason,bool(selection)),
   Row("profile",loc::T("home.profile"),loc::T("home.profile_detail")),
   Row("settings",loc::T("home.settings"),loc::T("home.settings_detail")),
   Row("about",loc::T("home.about"),loc::T("home.about_detail")),
   Row("offline",loc::T("home.offline"),loc::T("home.offline_detail"),idle)};
  if(opening)rows[0].detail=v.session.isHost?loc::T("room.creating_status"):loc::T("room.joining_status");
  if(!v.controllerReady)rows.insert(rows.begin(),Row("player",loc::T("home.choose_controller"),loc::T("home.choose_controller_detail")));
  // Back leaves a pending invitation's screen without answering it, so Home
  // keeps a way back to it until it is answered or expires.
  if(v.discordPending)rows.insert(rows.begin(),Row("discord-invitation",loc::T("discord.invitation_pending"),loc::T("discord.invitation_pending_detail")));
  if(developer)rows.push_back(Row("developer","Developer","Development tools."));
 }else if(screen=="profile"){
  title=loc::T("profile.title");const auto& record=v.preferences.record;
  rows={TextRow("name",loc::T("profile.player_name"),preferences_.displayName,31,v.canEditPreferences),
   Row("main-character",loc::T("profile.main_character"),loc::Tf("profile.main_character_detail",selection::FindFighter(preferences_.mainFighter)->name)),
   Row("record",loc::T("profile.record"),record.available?loc::Tf("profile.record_detail",record.wins,record.losses):loc::T("profile.record_unavailable"))};
  rows[2].info=true;
  rows[1].value=selection::FindFighter(preferences_.mainFighter)->name;
  rows[2].value=record.available?loc::Tf("profile.record_value",record.wins,record.losses):loc::T("common.unavailable");
 }else if(screen=="main-character"){
  title=loc::T("profile.choose_main_title");
  for(int id=0;id<selection::FighterCount;++id){rows.push_back(Row("main-"+std::to_string(id),selection::FindFighter(id)->name,v.canEditPreferences?loc::T("profile.choose_main_detail"):loc::T("profile.leave_room_to_edit"),v.canEditPreferences));
   rows.back().hint=loc::T("menu.hint.save_main");}
 }else if(screen=="online"){
  title=loc::T("online.title");rows={Row("create",loc::T("online.create"),loc::T("online.create_detail"),v.canOpenRoom),Row("join",loc::T("online.join"),loc::T("online.join_detail"),v.canOpenRoom),
   Row("relay",loc::T("network.relay"),loc::T("network.relay_detail")),Row("network",loc::T("network.status"),DescribeNatDetail(v.netReport))};
  // Where this PC connects and how its network treats a direct path, for information only.
  rows[2].info=rows[3].info=true;rows[2].value=DescribeRelay(v.netReport);rows[3].value=DescribeNat(v.netReport.nat);
 }else if(screen=="create"||screen=="defaults"){
  title=screen=="create"?loc::T("room.create_title"):loc::T("settings.gameplay_defaults_title");const bool can=screen=="create"?v.canOpenRoom:v.canEditPreferences;
  if(screen=="defaults")rows.push_back(Value("delay",loc::T("settings.input_delay"),std::to_string(preferences_.inputDelay),reason,can));
  rows.push_back(TextRow("room-name",loc::T("room.name"),preferences_.roomName,64,can));
  rows.push_back(Value("capacity",loc::T("room.capacity"),std::to_string(preferences_.roomCapacity),loc::T("room.capacity_detail"),can));
  RuleRows(rows,preferences_.tableRules,can,reason);
  if(screen=="create"){rows.push_back(opening?ConfirmRow("cancel-open",loc::T("room.stop_creating_action"),loc::T("room.stop_creating"),true):
   Row("host",loc::T("online.create"),loc::T("room.create_requirements"),can&&preferences_.Valid()));
   if(!opening)rows.back().hint=loc::T("online.create");}
 }else if(screen=="join"){
  title=loc::T("room.join_title");rows={Row("paste",loc::T("room.paste_invitation"),loc::T("room.paste_invitation_detail"),v.canOpenRoom),
   opening?ConfirmRow("cancel-open",loc::T("room.stop_joining_action"),loc::T("room.stop_joining"),true):Row("join-now",loc::T("online.join"),loc::T("room.join_pasted"),v.canOpenRoom&&invitation_[0]),
   TextRow("invite-text",loc::T("room.edit_invitation"),invitation_,sizeof(invitation_)-1,v.canOpenRoom)};
  rows[0].hint=loc::T("menu.hint.paste");if(!opening)rows[1].hint=loc::T("online.join");
 }else if(screen.compare(0,4,"room")==0){title=v.room.name.empty()?loc::T("screen.room"):v.room.name;NoteUserText(v.room.name);rows=RoomEntries(v);
 }else if(screen=="settings"){
  title=loc::T("settings.title");rows={Row("player",loc::T("screen.player"),loc::T("settings.player_detail")),Row("defaults",loc::T("screen.defaults"),loc::T("settings.defaults_detail")),Row("interface",loc::T("settings.interface"),loc::T("settings.interface_detail")),Row("discord",loc::T("screen.discord"),loc::T("settings.discord_detail"))};
 }else if(screen=="player"){
  title=loc::T("player.title");rows={TextRow("name",loc::T("profile.player_name"),preferences_.displayName,31,v.canEditPreferences),
   Row("capture",loc::T("player.change_controller"),loc::Tf("player.change_controller_detail",v.controller),v.canChangeController),
   ConfirmRow("keyboard",loc::T("player.use_keyboard"),loc::T("player.use_keyboard_detail"),v.canChangeController),
   Row("controls",loc::T("player.native_menus"),loc::T("player.native_menus_detail"),idle)};
 }else if(screen=="interface"){
  title=loc::T("settings.interface_title");char size[32];std::snprintf(size,sizeof(size),"%.2fx",preferences_.interfaceScale);
  const char* hudSizes[]={loc::T("size.small"),loc::T("size.standard"),loc::T("size.large")};
  const auto languageValue=languagePreference_=="auto"?loc::Tf("settings.language.system_with",loc::NativeName(loc::Active())):
   std::string(loc::NativeName(loc::ResolveLocale(languagePreference_,{},{})));
  rows={Value("hud",loc::T("settings.match_hud"),preferences_.showMatchHud?loc::T("common.on"):loc::T("common.off"),reason,v.canEditPreferences),
   Value("hud-size",loc::T("settings.match_hud_size"),hudSizes[(std::max)(0,(std::min)(2,preferences_.matchHudSize))],loc::T("settings.match_hud_size_detail"),v.canEditPreferences),
   Value("hud-spacing",loc::T("settings.bottom_spacing"),preferences_.matchHudRaised?loc::T("spacing.raised"):loc::T("spacing.normal"),loc::T("settings.bottom_spacing_detail"),v.canEditPreferences),
   Value("ready-sound",loc::T("settings.ready_sound"),preferences_.readySound?loc::T("common.on"):loc::T("common.off"),loc::T("settings.ready_sound_detail"),v.canEditPreferences),
   Value("ready-volume",loc::T("settings.ready_sound_volume"),std::to_string(preferences_.readySoundVolume)+"%",loc::T("settings.ready_sound_volume_detail"),v.canEditPreferences&&preferences_.readySound),
   Row("ready-test",loc::T("settings.ready_sound_test"),loc::T("settings.ready_sound_test_detail"),v.canEditPreferences&&preferences_.readySound),
   Value("scale",loc::T("settings.interface_size"),size,reason,v.canEditPreferences),
   Value("language",loc::T("settings.language"),languageValue,languageSaveError_.empty()?std::string(loc::T("settings.language.detail")):languageSaveError_,true)};
  // Select lists the languages by their own names; browsing them changes nothing.
  rows.back().choices=LanguageChoices();rows.back().chosen=languagePreference_;
 }else if(screen=="discord"){
  title=loc::T("discord.title");rows={Value("presence",loc::T("discord.show_activity"),preferences_.discordPresence?loc::T("common.on"):loc::T("common.off"),v.discordStatus,v.canEditPreferences),
   Value("invites",loc::T("discord.allow_invitations"),preferences_.discordInvites?loc::T("common.on"):loc::T("common.off"),preferences_.discordPresence?loc::T("discord.invitation_detail"):loc::T("discord.enable_first"),v.canEditPreferences&&preferences_.discordPresence)};
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
  using platform::ServiceAction;
  // Each row shows the service message only for the actions it requests.
  const auto outcome=[&](std::initializer_list<ServiceAction> own,std::string idle){
   for(const auto action:own)if(v.services.lastAction==action&&!v.services.message.empty())return v.services.message;
   return idle;
  };
  // The controls, credits and licence are longer than the detail pane, so
  // Select opens them in a reader a pad or keyboard can scroll.
  auto reading=[](MenuEntry e){e.reading=true;return e;};
  title=loc::T("about.title");rows={reading(Row("help",loc::T("about.controls"),std::string(loc::T("about.controls_detail"))+"\n\n"+loc::T("about.controls_keyboard"))),
   reading(Row("credits",loc::T("about.ember"),loc::Tf("about.ember_detail",v.build))),
   reading(Row("font",loc::T("about.font_license"),FontLicense())),Row("diagnostics",loc::T("about.export_diagnostics"),
    outcome({ServiceAction::ExportDiagnostics},loc::T("about.export_diagnostics_detail")),!v.services.pending),
   ConfirmRow("community",loc::T("about.discord"),
    outcome({ServiceAction::OpenCommunity},loc::Tf("about.discord_detail",platform::CommunityInvite)),!v.services.pending),
   Row("updates",loc::T("updates.check"),outcome({ServiceAction::CheckUpdates,ServiceAction::InstallUpdate,ServiceAction::OpenUpdater,ServiceAction::OpenRecovery},
    loc::T("about.updates_detail")),!v.services.pending)};
  if(v.services.update.ok&&v.services.update.updateAvailable)rows.push_back(ConfirmRow("updater",loc::T("about.open_updater"),loc::T("about.open_updater_detail"),v.canEditPreferences&&!v.services.pending));
  if(v.network==NetworkAvailability::Unavailable)rows.push_back(ConfirmRow("recovery",loc::T("about.open_recovery"),
   v.canEditPreferences?loc::T("about.open_recovery_detail"):loc::T("about.leave_room_first"),v.canEditPreferences&&!v.services.pending));
 }
 return rows;
}
std::pair<std::string,Tone> ApplicationShell::UpdateStatus(const ShellView& v,const std::string& screen,bool opening,bool healthyRoom,std::string& title) {
 using namespace netplay;
 const bool personal=screen=="profile"||screen=="main-character"||screen=="settings"||screen=="player"||screen=="defaults"||screen=="interface"||screen=="discord";
 std::string status=saveFailed_?loc::T("common.save_failed"):v.settingsPending||preferencesDirty_||saveQueued_||languageDirty_?loc::T("common.saving"):personal?loc::T("common.saved"):"";
 // Severity travels with the status string. This line is the shell's only
 // feedback channel, so a failure must not render like ordinary text.
 Tone statusTone=saveFailed_?Tone::Error:v.settingsPending||preferencesDirty_||saveQueued_||languageDirty_?Tone::Pending:
  personal?Tone::Success:Tone::Neutral;
 if((screen=="create"||screen=="join"||screen=="home"||screen=="online")&&opening&&status.empty()){
  status=v.session.isHost?loc::T("room.creating_status"):loc::T("room.joining_status");statusTone=Tone::Pending;
 }
 if(screen=="room"&&status.empty()){
  const bool healthy=v.session.control==Health::Healthy;
  status=healthy?(v.room.locked?loc::T("room.locked_status"):loc::T("room.private_status")):loc::T("room.reconnecting");
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
    (table.ready[side]&&!finishedGame?loc::T("room.ready_suffix"):local&&v.readyRequested?loc::T("room.readying_suffix"):loc::T("room.not_ready_suffix"));
  };
  status=state(table.p1,0)+" | "+state(table.p2,1);
  // Table phases carry their own tone: an unresolved result is a problem
  // the player must act on, a pending result or preparation is a wait.
  if(table.phase==room::TablePhase::Paused){status=loc::T("room.result_unresolved_status");statusTone=Tone::Error;}
  else if(table.phase==room::TablePhase::Ready){status=loc::T("room.preparing_status");statusTone=Tone::Pending;}
  else if(table.phase==room::TablePhase::Playing&&!finishedGame){
   status=table.resultPending?loc::T("room.waiting_results_status"):loc::T("room.match_in_progress");
   statusTone=Tone::Pending;
  }
  else if(table.phase==room::TablePhase::Closed)status=loc::T("room.table_closed");
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
 // The failed language save is about the row on the Interface screen, so it
 // speaks there only, and every other report below outranks it.
 if(!languageSaveError_.empty()&&screen=="interface"){status=languageSaveError_;statusTone=Tone::Error;}
 if(v.controllerUnavailable){status=loc::T("controller.disconnected");statusTone=Tone::Error;}
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
 PlayerCardView card;card.name=preferences_.displayName;card.fighter=preferences_.mainFighter;
 card.fighterName=selection::FindFighter(preferences_.mainFighter)->name;card.inputDelay=preferences_.inputDelay;
 card.wins=v.preferences.record.wins;card.losses=v.preferences.record.losses;card.recordAvailable=v.preferences.record.available;
 card.controllerReady=v.controllerReady;card.connected=v.session.control==Health::Healthy;
 card.members=static_cast<int>(v.room.members.size());
 for(const auto& table:v.room.tables)if(table.phase==room::TablePhase::Playing)++card.activeTables;
 SetMenuPlayerCard(std::move(card));
}
void ApplicationShell::HandleActivate(const MenuAction& a,const ShellView& v,const std::string& screen,bool idle,const Submit& submit) {
 using namespace netplay; auto& nav=menu_.navigation;
 // An opening room keeps its own screen, with its Stop row, until it joins.
 if(a.id=="online")nav.Push(idle?"online":v.session.room==RoomState::Opening?OpeningScreen(v):"room");
 else if(a.id=="discord-invitation")nav.Push(a.id);
 else if(a.id=="profile"||a.id=="main-character")nav.Push(a.id);
 else if(a.id.compare(0,5,"main-")==0&&v.canEditPreferences){preferences_.mainFighter=std::stoi(a.id.substr(5));preferencesDirty_=true;profileSavePending_=true;error_.clear();saveAt_=ImGui::GetTime()+.45;}
 else if(a.id=="selection"){selectionFresh_=true;changeFighter_=screen.compare(0,4,"room")==0;nav.Push(a.id);}
 else if(a.id=="settings"||a.id=="about"||a.id=="create"||a.id=="join"||a.id=="player"||a.id=="defaults"||a.id=="interface"||a.id=="discord"||a.id=="developer")nav.Push(a.id);
 else if(a.id=="host"||a.id=="join-now")Send(a.id=="host"?CommandKind::HostRoom:CommandKind::JoinInvite,v,submit);
 else if(a.id=="cancel-open")Send(CommandKind::LeaveRoom,v,submit);
 else if(a.id=="offline"||a.id=="controls")Send(CommandKind::StartOffline,v,submit);
 else if(a.id=="paste"){const char* t=ImGui::GetClipboardText();if(t&&*t&&std::strlen(t)<sizeof(invitation_)){std::strcpy(invitation_,t);error_.clear();}else error_=loc::T("error.invitation_invalid");}
 else if(a.id=="capture"||a.id=="keyboard"){ShellAction r;r.command.generation=v.session.generation;r.inputAction=a.id=="capture"?input::Action::BeginCapture:input::Action::UseKeyboard;submit(std::move(r));}
 else if(a.id=="invite-cancel"||a.id=="invite-switch"){ShellAction r;r.command.generation=v.session.generation;r.discordRevision=v.discordRevision;r.discordAction=a.id=="invite-cancel"?discord::InviteAction::Cancel:discord::InviteAction::Switch;if(!submit(std::move(r)))error_=loc::T("error.invitation_changed");}
 else if(a.id=="retry-save"){saveFailed_=false;retrySave_=true;preferencesDirty_=true;saveAt_=0;error_.clear();}
 else if(a.id=="diagnostics")Service(platform::ServiceAction::ExportDiagnostics,v,submit);
 else if(a.id=="ready-test"){
  // The volume on screen, which may not be saved yet.
  ShellAction r;r.command.generation=v.session.generation;r.previewSoundVolume=preferences_.readySoundVolume;
  if(!submit(std::move(r)))error_=loc::T("error.queue_failed");
 }
 else if(a.id=="updates")Service(platform::ServiceAction::CheckUpdates,v,submit);
 else if(a.id=="updater")Service(platform::ServiceAction::OpenUpdater,v,submit);
 else if(a.id=="recovery")Service(platform::ServiceAction::OpenRecovery,v,submit);
 else if(a.id=="community")Service(platform::ServiceAction::OpenCommunity,v,submit);
 else if(screen.compare(0,4,"room")==0)RoomAction(a,v,submit);
}
void ApplicationShell::HandleAdjust(const MenuAction& a,const ShellView& v,const std::string& screen,const Submit& submit) {
 using namespace netplay;
 if(screen.compare(0,4,"room")==0)RoomAction(a,v,submit);
 else if(a.id=="invite-text")std::snprintf(invitation_,sizeof(invitation_),"%s",a.text.c_str());
 else if(a.id=="language")SetLanguage(std::string(loc::NextPreference(languagePreference_,a.delta)));
 else{
  auto prior=preferences_;
  if(a.id=="name")preferences_.displayName=a.text;else if(a.id=="room-name")preferences_.roomName=a.text;
  else if(a.id=="capacity")preferences_.roomCapacity=(std::max)(2,(std::min)(16,preferences_.roomCapacity+a.delta));
  else if(a.id=="delay")preferences_.inputDelay=(std::max)(0,(std::min)(10,preferences_.inputDelay+a.delta));
  else if(a.id=="hud-size")preferences_.matchHudSize=(std::max)(0,(std::min)(2,preferences_.matchHudSize+a.delta));
  else if(a.id=="hud-spacing")preferences_.matchHudRaised=a.delta>0;
  else if(a.id=="ready-sound")preferences_.readySound=a.delta>0;
  else if(a.id=="ready-volume")preferences_.readySoundVolume=(std::max)(10,(std::min)(100,preferences_.readySoundVolume+10*a.delta));
  else if(a.id=="scale")preferences_.interfaceScale=(std::max)(1.f,(std::min)(1.5f,preferences_.interfaceScale+.05f*a.delta));
  else if(a.id=="hud")preferences_.showMatchHud=a.delta>0;else if(a.id=="presence")preferences_.discordPresence=a.delta>0;
  else if(a.id=="invites")preferences_.discordInvites=a.delta>0;else AdjustRule(preferences_.tableRules,a);
  if(!preferences_.Valid()){preferences_=prior;error_=loc::T("error.invalid_value");}
  else{preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;error_.clear();}
 }
}

void ApplicationShell::SetLanguage(std::string preference) {
 // Not a netplay preference: it lives in the UI preferences file and saves on
 // its own deadline, so it stays out of the validate-and-save tail of HandleAdjust.
 if(!loc::ValidPreference(preference))return;
 languagePreference_=std::move(preference);
 loc::SetActive(platform::ResolveUiLocale(languagePreference_));
 languageDirty_=true;languageSaveError_.clear();languageSaveAt_=ImGui::GetTime()+.45;
}
void ApplicationShell::Draw(const ShellView& v,bool* open,const Submit& submit,const DrawSelection& selection,const DrawSelection& developer) {
 using namespace netplay; auto& nav=menu_.navigation;
 const double now = ImGui::GetTime();
 if(!languageSeeded_){languagePreference_=loc::ValidPreference(v.languagePreference)?v.languagePreference:"auto";languageSeeded_=true;}
 if(lastUiTime_ >= 0 && now < lastUiTime_) {
  // DX9 reset recreates ImGui, but these deadlines belong to the surviving shell.
  // Keep raw-clock users in RoomAction in the same epoch, including queued saves.
  saveAt_ = RebaseUiTimestamp(saveAt_, lastUiTime_, now);
  languageSaveAt_ = RebaseUiTimestamp(languageSaveAt_, lastUiTime_, now);
  noticeUntil_ = RebaseUiTimestamp(noticeUntil_, lastUiTime_, now);
  roomUpdateUntil_ = RebaseUiTimestamp(roomUpdateUntil_, lastUiTime_, now);
  roomUpdateStarted_ = -1;
 }
 lastUiTime_ = now;
 // Player-written text can need glyphs the atlas has not baked yet. The
 // renderer tells the theme about each string it draws, where it draws it, so
 // text that is scrolled out of view, muted or on another screen holds none.
 if(!gameSettingsChecked_&&v.showGameSettingsCard){
  // Once per launch. "Don't show again" is the card's own outcome, so it
  // travels with the notice; a failed save just means the card returns.
  gameSettingsChecked_=true;
  std::string advice=GameSettingsAdvice(v.gameSettings);
  if(!advice.empty())
   menu_.ShowNotice(std::move(advice),loc::T("game_settings.title"),loc::T("game_settings.hide"),
    []{std::string diagnostic;platform::HideGameSettingsCardForever(diagnostic);});
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
 TrackLiveGames(v,now);
 const bool healthyRoom=UpdateRoomFeedback(v);
 UpdatePreferenceSave(v,submit);
 if(v.readyFailureSequence!=readyFailureSequence_){
  readyFailureSequence_=v.readyFailureSequence;
  if(readyFailureSequence_&&!v.readyFailure.empty())menu_.ShowNotice(v.readyFailure);
 }
 if(v.discordPending&&inviteRevision_!=v.discordRevision){inviteRevision_=v.discordRevision;nav.Push("discord-invitation");}
 if(!v.discordPending&&nav.Screen()=="discord-invitation")nav.Return();
 if(v.inputCapture!=input::Capture::Idle&&nav.Screen()!="assignment")nav.Push("assignment");
 if(v.inputCapture==input::Capture::Idle&&nav.Screen()=="assignment")nav.Return();
 const auto* vp=ImGui::GetMainViewport();ImGui::SetNextWindowPos(vp->Pos);ImGui::SetNextWindowSize(vp->Size);
 ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20*Scale(),16*Scale()));ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
 ImGui::Begin("SF4 Ember Netplay###EmberShell",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs);
 // Room shortcuts work from every screen of a joined room, including the
 // embedded fighter selector, which forwards what it does not handle.
 const bool inRoom=v.session.room!=RoomState::Idle&&v.room.roomEpoch;
 const bool keys=KeyboardPrompts();
 std::vector<LegendHint> roomHints=keys?std::vector<LegendHint>{{"F",loc::T("room.legend_fighter")},{"T",loc::T("room.legend_options")},{"C",loc::T("room.chat")}}:
  std::vector<LegendHint>{{"X",loc::T("room.legend_fighter")},{"Y",loc::T("room.legend_options")},{"Back/Select",loc::T("room.chat")}};
 if(nav.Screen()=="selection"&&selection){
  // The selector names where its Back goes and shows the room's shortcuts it hands back.
  SetEmbeddedReturn({MenuScreenLabel(nav.Parent()),inRoom?roomHints:std::vector<LegendHint>{},selectionFresh_,changeFighter_});
  selectionFresh_=changeFighter_=false;
  // Only what this frame's selector forwards is read below.
  TakeForwardedMenuAction();
  selection();
  const auto forwarded=TakeForwardedMenuAction();
  if(forwarded.kind==MenuAction::Close)nav.Return();
  else if(forwarded.kind==MenuAction::Shortcut&&inRoom)RoomShortcut(forwarded,v);
  ImGui::End();ImGui::PopStyleVar(2);return;
 }
 const std::string screen=nav.Screen();std::string title=loc::T("shell.home_title");
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
 PublishPlayerCard(v);
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
  ImGui::TextUnformatted(loc::T("settings.preview"));
  MatchStripView preview;preview.names[0]=loc::T("settings.player_one");preview.names[1]=loc::T("settings.player_two");
  preview.pingMs=68;preview.rollbackFrames=2;preview.appliedDelay=3;
  preview.size=preferences_.matchHudSize;preview.raised=preferences_.matchHudRaised;
  DrawMatchStripPreview(preview);
 };
 if(screen=="room"&&v.room.roomEpoch)board=[&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction& action,float height,const MenuVisualFeedback& feedback){DrawRoomBoard(v,entries,navigation,action,height,feedback);};
 // Visual grace cannot grant permission: enabled and all dispatch checks stay live.
 const bool checkpointPending=roomScreen && RoomCheckpointPending(v) && !v.controllerUnavailable &&
  SessionProblem(v.session).empty() && v.error.empty() && error_.empty();
 for(auto& row:rows)row.pending=checkpointPending;
 // Every shell screen except Home reserves the status area, so a status that
 // grows can never displace the list under a highlight or a mouse click.
 // Home renders its status in the small-print line below the list instead.
 const bool stableFeedback=screen!="home";
 if(roomScreen&&v.room.roomEpoch)menu_.shortcutHints=roomHints;
 else menu_.shortcutHints.clear();
 // The keyboard leaves a seat with Delete, so Escape keeps its own word.
 const char* placeExit=screen=="room"&&v.room.roomEpoch?PlaceExitLabel(v):"";
 if(keys&&*placeExit)menu_.shortcutHints.insert(menu_.shortcutHints.begin(),{"Del",placeExit});
 // Back names where it goes when that is not simply the previous screen:
 // from Home, out of Ember or back to the room; on the board, out of your seat.
 menu_.exitName.clear();menu_.backHint.clear();
 if(screen=="home"){
  menu_.exitName=opening?MenuScreenLabel(OpeningScreen(v)):loc::T("screen.game");
  menu_.backHint=loc::Tf("menu.return_to",menu_.exitName);
 }else if(!keys)menu_.backHint=placeExit;
 // While a controller is being captured it cannot drive the menu, so the
 // legend shows the keyboard and names the cancel.
 if(v.inputCapture!=input::Capture::Idle){SetMenuGlyphs(input::PadKeyboard,0,0);menu_.backHint=loc::T("common.cancel");}
 auto a=menu_.Draw(title.c_str(),rows,status.c_str(),profilePreview,columns,portraits,board,0,100,stableFeedback,statusTone,screen=="home");
 if(v.inputCapture!=input::Capture::Idle&&(a.id=="capture-cancel"||a.kind==MenuAction::Returned||a.kind==MenuAction::Close)){
  ShellAction r;r.command.generation=v.session.generation;r.inputAction=input::Action::Cancel;submit(std::move(r));
 }else if(a.kind==MenuAction::Close&&opening){
  // A room being opened keeps its Stop row in reach.
  nav.Push(OpeningScreen(v));
 }else if(a.kind==MenuAction::Close||a.id=="return"){
  // Back at the root hides Ember; a joined room is kept, and the Ember shortcut returns to it.
  if(open)*open=false;
 }else if(a.kind==MenuAction::Shortcut){
  if(inRoom)RoomShortcut(a,v);
 }else if(a.kind==MenuAction::Chosen){
  if(roomScreen)RoomAction(a,v,submit);
  else if(a.id=="language")SetLanguage(a.text);
 }else if(a.kind==MenuAction::Activate){
  HandleActivate(a,v,screen,idle,submit);
 }else if(a.kind==MenuAction::Adjust||a.kind==MenuAction::TextAccepted){
  HandleAdjust(a,v,screen,submit);
 }
 ImGui::End();ImGui::PopStyleVar(2);
}
} }
