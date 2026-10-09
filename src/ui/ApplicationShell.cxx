#include "ApplicationShell.hxx"
#include "Theme.hxx"
#include "MenuRows.hxx"
#include "RoomFeedback.hxx"
#include "NetworkFeedback.hxx"
#include "MenuPresentation.hxx"
#include "PublicRoomsCards.hxx"
#include "RoomControls.hxx"
#include "../session/TrainingCall.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
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
// `text` cut to at most `limit` bytes, never inside a character.
std::string CutUtf8(std::string text,std::size_t limit) {
    if(text.size()<=limit)return text;
    while(limit>0&&(static_cast<unsigned char>(text[limit])&0xC0)==0x80)--limit;
    text.resize(limit);return text;
}
// The width of the list: the whole window on a narrow one, else the left pane.
float PublicListWidth() {
    const float available=ImGui::GetContentRegionAvail().x;
    return available>=820*Scale()?available*.53f:available;
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
bool ApplicationShell::Send(netplay::CommandKind kind, const ShellView& view, const Submit& submit, bool enterTraining) {
    ShellAction action;
    action.command.kind = kind;
    action.enterTraining = enterTraining && kind == netplay::CommandKind::StartOffline;
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
void ApplicationShell::CopyShortInvitation(const ShellView& v,const Submit& submit) {
 error_.clear();
 if(!v.shortInvitation.empty()){
  ImGui::SetClipboardText(v.shortInvitation.c_str());
  notice_=loc::T("room.short_invitation_copied");noticeTone_=Tone::Success;noticeUntil_=ImGui::GetTime()+3;shortCopyPending_=false;
  return;
 }
 ShellAction request;request.command.generation=v.session.generation;request.shortInvitation=true;
 shortFailuresSeen_=v.shortInvitationFailures;
 shortCopyPending_=true;shortCopyUntil_=ImGui::GetTime()+12;
 // A request that cannot be queued falls back at once, in UpdateShortCopy.
 if(!submit(std::move(request)))shortCopyUntil_=0;
 notice_=loc::T("room.short_invitation_pending");noticeTone_=Tone::Pending;noticeUntil_=ImGui::GetTime()+12;
}
// The short link answers within a few seconds. Without one, the full
// invitation goes to the clipboard so the press still shares the room.
void ApplicationShell::UpdateShortCopy(const ShellView& v,double now) {
 if(!shortCopyPending_)return;
 if(v.session.room==netplay::RoomState::Idle||v.invitation.empty()){shortCopyPending_=false;return;}
 if(!v.shortInvitation.empty()){
  shortCopyPending_=false;ImGui::SetClipboardText(v.shortInvitation.c_str());
  notice_=loc::T("room.short_invitation_copied");noticeTone_=Tone::Success;noticeUntil_=now+3;
  return;
 }
 if(v.shortInvitationFailures==shortFailuresSeen_&&now<shortCopyUntil_)return;
 shortCopyPending_=false;ImGui::SetClipboardText(v.invitation.c_str());
 notice_=loc::T("room.short_invitation_unavailable");noticeTone_=Tone::Neutral;noticeUntil_=now+6;
}
// A room link opened from the browser joins its room at once when the
// player is free, as soon as a room can be opened. While a room is open it
// waits, so a link never moves the player out of a room or a match; that
// link, or one the runtime no longer offers for a direct join, fills the
// Join screen and joining is the player's own press. Neither happens over
// a dialog, a notice, an Ember ID screen, whose drafts the move would discard,
// or a public room being asked for.
void ApplicationShell::UpdateJoinLink(const ShellView& v,double now,const Submit& submit) {
 using namespace netplay; auto& nav=menu_.navigation;
 const bool free=v.session.room==RoomState::Idle&&v.session.match==MatchState::None;
 if(v.pendingJoinSequence!=joinLinkSeen_){
  joinLinkSeen_=v.pendingJoinSequence;joinLink_=v.pendingJoinLink;
  joinLinkDirect_=free&&v.pendingJoinDirect;
  if(!joinLink_.empty()&&v.session.room!=RoomState::Idle){notice_=loc::T("room.link_waiting");noticeTone_=Tone::Pending;noticeUntil_=now+8;}
 }
 if(joinLink_.empty())return;
 if(!free){joinLinkDirect_=false;return;}
 const bool direct=joinLinkDirect_&&v.pendingJoinDirect;
 if(direct&&!v.canOpenRoom)return;
 if(nav.Editing()||nav.Reading()||nav.Confirming()||nav.Choosing()||menu_.NoticeOpen()||IdentityPanel::Owns(nav.Screen())||publicRooms_.Busy())return;
 std::snprintf(invitation_,sizeof(invitation_),"%s",joinLink_.c_str());joinLink_.clear();joinLinkDirect_=false;
 nav.Cancel();nav.Home();nav.Push("online");nav.Push("join");
 error_.clear();noticeTone_=Tone::Neutral;noticeUntil_=now+8;
 if(direct&&Send(CommandKind::JoinInvite,v,submit)){notice_=loc::T("room.link_joining");noticeTone_=Tone::Pending;}
 else notice_=loc::T("room.link_opened");
}
// A public room link opens Public rooms on its service and asks for its room
// (PublicRoomsPanel). Like a match link it never moves a player who is in a
// room or a game: it waits there as a row, and says so. A link that arrived
// while the player was free also waits, for as long as a dialog, a notice or
// an Ember ID screen, whose drafts the move would discard, is open.
void ApplicationShell::UpdatePublicRoomLink(const ShellView& v,double now) {
 using namespace netplay; auto& nav=menu_.navigation;
 const auto& link=v.tournament.roomLink;
 if(link.sequence!=roomLinkSeen_){
  roomLinkSeen_=link.sequence;
  roomLinkOpening_=link.free&&v.session.room==RoomState::Idle&&v.session.match==MatchState::None;
  publicRooms_.OpenLink(link.bridge,link.room,roomLinkOpening_);
  if(!roomLinkOpening_){notice_=loc::T("public.link_waiting");noticeTone_=Tone::Pending;noticeUntil_=now+15;}
 }
 if(!roomLinkOpening_)return;
 // Busy by the time it can go on (a hidden menu, a dialog): it waits as a row instead.
 if(v.session.room!=RoomState::Idle||v.session.match!=MatchState::None){roomLinkOpening_=false;publicRooms_.HoldLink();return;}
 if(nav.Editing()||nav.Reading()||nav.Confirming()||nav.Choosing()||menu_.NoticeOpen()||IdentityPanel::Owns(nav.Screen()))return;
 roomLinkOpening_=false;
 identity_.Probe(); // the trusted services, fresh, before the link is judged
 if(nav.Screen()!="public-rooms"){nav.Cancel();nav.Home();nav.Push("online");nav.Push("public-rooms");}
 error_.clear();
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
   Row("identity",loc::T("screen.identity"),identity_.HomeDetail(v)),
   Row("settings",loc::T("home.settings"),loc::T("home.settings_detail")),
   Row("offline",loc::T("home.offline"),loc::T("home.offline_detail"),idle),
   Row("training",loc::T("home.training"),loc::T("home.training_detail"),idle),
   Row("replays",loc::T("home.replays"),loc::T("home.replays_detail"))};
  if(opening)rows[0].detail=OpeningCreates(v)?loc::T("room.creating_status"):loc::T("room.joining_status");
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
  for(const int id:selection::RosterDisplayOrder){rows.push_back(Row("main-"+std::to_string(id),selection::FindFighter(id)->name,v.canEditPreferences?loc::T("profile.choose_main_detail"):loc::T("profile.leave_room_to_edit"),v.canEditPreferences));
   rows.back().hint=loc::T("menu.hint.save_main");}
 }else if(screen=="online"){
  title=loc::T("online.title");rows={Row("create",loc::T("online.create"),loc::T("online.create_detail"),v.canOpenRoom),Row("join",loc::T("online.join"),loc::T("online.join_detail"),v.canOpenRoom),
   Row("public-rooms",loc::T("screen.public_rooms"),loc::T("online.public_detail"),v.canOpenRoom),
   Row("relay",loc::T("network.relay"),loc::T("network.relay_detail")),Row("network",loc::T("network.status"),DescribeNatDetail(v.netReport))};
  // Where this PC connects and how its network treats a direct path, for information only.
  rows[3].info=rows[4].info=true;rows[3].value=DescribeRelay(v.netReport);rows[4].value=DescribeNat(v.netReport.nat);
 }else if(screen=="create"||screen=="defaults"){
  title=screen=="create"?loc::T("room.create_title"):loc::T("settings.gameplay_defaults_title");const bool can=screen=="create"?v.canOpenRoom:v.canEditPreferences;
  // A public room is made by the service with the public default rules; the
  // ones chosen here follow once the creator is in it as host (netplay::CreatedRules).
  const bool publicRoom=screen=="create"&&preferences_.roomPublic;
  const std::string bridge=publicRoom?identity_.UsableBridge(v):std::string();
  if(screen=="defaults")rows.push_back(preferences_.autoInputDelay?Value("delay",loc::T("settings.input_delay"),loc::T("settings.input_delay.auto"),
   loc::Tf("room.input_delay.auto.detail",AutoInputDelayMinimum,AutoInputDelayMaximum)+"\n"+reason,can):
   Value("delay",loc::T("settings.input_delay"),std::to_string(preferences_.inputDelay),reason,can));
  if(screen=="create")rows.push_back(Value("visibility",loc::T("public.visibility"),loc::T(publicRoom?"public.visibility.public":"public.visibility.private"),loc::T("public.visibility_detail"),can));
  rows.push_back(TextRow("room-name",loc::T("room.name"),preferences_.roomName,64,can));
  rows.push_back(Value("capacity",loc::T("room.capacity"),std::to_string(preferences_.roomCapacity),loc::T("room.capacity_detail"),can));
  RuleRows(rows,publicRoom?preferences_.publicTableRules:preferences_.tableRules,can,reason);
  if(screen=="create"){rows.push_back(opening?ConfirmRow("cancel-open",loc::T("room.stop_creating_action"),loc::T("room.stop_creating"),true):
   Row("host",loc::T(publicRoom?"public.create":"online.create"),loc::T(!publicRoom?"room.create_requirements":bridge.empty()?"public.create_needs_id":"public.create_requirements"),
    can&&preferences_.Valid()&&!publicRooms_.Busy()&&(!publicRoom||!bridge.empty())));
   if(!opening)rows.back().hint=loc::T(publicRoom?"public.create":"online.create");}
 }else if(screen=="join"){
  title=loc::T("room.join_title");rows={Row("paste",loc::T("room.paste_invitation"),loc::T("room.paste_invitation_detail"),v.canOpenRoom),
   opening?ConfirmRow("cancel-open",loc::T("room.stop_joining_action"),loc::T("room.stop_joining"),true):Row("join-now",loc::T("online.join"),loc::T("room.join_pasted"),v.canOpenRoom&&invitation_[0]),
   TextRow("invite-text",loc::T("room.edit_invitation"),invitation_,sizeof(invitation_)-1,v.canOpenRoom)};
  rows[0].hint=loc::T("menu.hint.paste");if(!opening)rows[1].hint=loc::T("online.join");
 }else if(PublicRoomsPanel::Owns(screen)){
  title=loc::T("screen.public_rooms");rows=publicRooms_.Rows(v,identity_.UsableBridge(v),identity_.Waiting(),identity_.PublicSetup(),PublicListWidth());
 }else if(screen.compare(0,4,"room")==0){title=v.room.name.empty()?loc::T("screen.room"):v.room.name;NoteUserText(v.room.name);rows=RoomEntries(v);
 }else if(screen=="replay-inputs"){title=loc::T("replays.inputs");BuildInputsRows(v,rows);
 }else if(screen=="replay-export"){
  // Each part of the caption on or off, with its text; Generate sends it with the export.
  title=loc::T("export.title");
  const auto onOff=[](bool on){return std::string(on?loc::T("common.on"):loc::T("common.off"));};
  const int one=1,two=2;
  for(const auto* text:{&caption_.name[0],&caption_.name[1],&caption_.text})NoteUserText(*text);
  rows={Value("cap-names",loc::T("export.names"),onOff(caption_.names),loc::T("export.detail")),
   TextRow("cap-name1",loc::Tf("export.name",one),caption_.name[0],31,caption_.names),
   TextRow("cap-name2",loc::Tf("export.name",two),caption_.name[1],31,caption_.names),
   Value("cap-line",loc::T("export.line"),onOff(caption_.line),loc::T("export.detail")),
   TextRow("cap-text",loc::T("export.line_text"),caption_.text,60,caption_.line),
   Value("cap-set",loc::T("export.set"),onOff(caption_.set),loc::T("export.set_detail")),
   Value("cap-set1",loc::Tf("export.set_wins",one),std::to_string(caption_.wins[0]),loc::T("export.set_detail"),caption_.set),
   Value("cap-set2",loc::Tf("export.set_wins",two),std::to_string(caption_.wins[1]),loc::T("export.set_detail"),caption_.set),
   Value("cap-mark",loc::T("export.mark"),onOff(caption_.mark),loc::T("export.detail")),
   Row("cap-generate",loc::T("export.generate"),loc::T("replays.export_gpu_detail"),idle&&v.replaysReady)};
 }else if(screen=="replays"){
  title=loc::T("replays.title");
  if(!v.replayLink.empty()){
   // A link names a file and nothing is played on its word: Select asks.
   rows.push_back(Row("replay-link",loc::T("replays.link"),loc::Tf("replays.link_detail",v.replayLink)));
   rows.back().detailText=DetailText::Name;NoteUserText(v.replayLink);
   rows.back().choices={{"link-play",loc::T("replays.link_play"),loc::T("replays.watch_detail"),idle&&v.replaysReady},{"link-dismiss",loc::T("replays.link_dismiss"),loc::T("replays.link_dismiss_detail")}};
   rows.back().chosen=idle&&v.replaysReady?"link-play":"link-dismiss";
  }
  rows.push_back(Row("replay-log",loc::T("replays.open_log"),loc::T(idle?"replays.open_log_detail":"replays.open_log_room"),idle&&v.replaysReady));
  rows.push_back(Row("replay-folder",loc::T("replays.open_folder"),v.services.lastAction==platform::ServiceAction::OpenReplayFolder&&!v.services.message.empty()?v.services.message:loc::T("replays.open_folder_detail"),!v.services.pending));
  if(v.replays.empty())rows.push_back(InfoRow("replay-none",loc::T("replays.empty"),"",loc::T("replays.empty_detail")));
  // A row is its file, not its place: the list is listed again while a row's choices are open.
  // A row is always open: reading its inputs needs nothing of the game. What does is each choice's own.
  for(std::size_t i=0;i<v.replays.size();i++){rows.push_back(Row("replay:"+v.replays[i].path,v.replays[i].label,loc::T(v.replaysReady?"replays.row_detail":"replays.not_ready")));
   // The label carries the players' own names. The value says what Ember knows about it.
   rows.back().userText=true;for(const auto& name:v.replays[i].names)NoteUserText(name);
   rows.back().value=v.replays[i].watched&&v.replays[i].spectated?loc::T("replays.watched_spectated"):v.replays[i].watched?loc::T("replays.watched"):v.replays[i].spectated?loc::T("replays.spectated"):"";
   if(v.replays[i].video)rows.back().value=rows.back().value.empty()?loc::T("replays.video"):rows.back().value+", "+loc::T("replays.video");
   // The score leads the value; the replay's own account of the match leads the detail.
   if(const auto& summary=v.replays[i].summary){
    const std::string score=ScoreText(*summary);
    if(!score.empty())rows.back().value=rows.back().value.empty()?score:score+" · "+rows.back().value;
    std::string said=MatchText(*summary);
    for(int side=0;side<2;side++)said+="\n"+ReplayPlayerName(v.replays[i].names,side)+": "+LookText(summary->players[side]);
    rows.back().detail=said+"\n\n"+rows.back().detail;rows.back().detailText=DetailText::Name;
   }
   // Select offers playback, adding to the Battle Log, exporting video, or reading inputs.
   rows.back().choices={{"watch",loc::T("replays.watch"),loc::T("replays.watch_detail"),idle&&v.replaysReady},{"watch-meter",loc::T("replays.watch_meter"),loc::T("replays.watch_meter_detail"),idle&&v.replaysReady},{"add",loc::T("replays.add"),loc::T("replays.add_detail"),v.replaysReady},{"export",loc::T("replays.export_gpu"),loc::T("replays.export_gpu_detail"),idle&&v.replaysReady},{"inputs",loc::T("replays.inputs"),loc::T("replays.inputs_detail")}};
   rows.back().chosen=!v.replaysReady?"inputs":idle?"watch":"add";}
 }else if(screen=="settings"){
  title=loc::T("settings.title");rows={Row("player",loc::T("screen.player"),loc::T("settings.player_detail")),Row("defaults",loc::T("screen.defaults"),loc::T("settings.defaults_detail")),Row("interface",loc::T("settings.interface"),loc::T("settings.interface_detail")),Row("training-replays",loc::T("settings.training_replays"),loc::T("settings.training_replays_detail")),Row("discord",loc::T("screen.discord"),loc::T("settings.discord_detail")),
   Row("about",loc::T("home.about"),loc::T("home.about_detail"))};
 }else if(screen=="player"){
  title=loc::T("player.title");rows={TextRow("name",loc::T("profile.player_name"),preferences_.displayName,31,v.canEditPreferences),
   Row("capture",loc::T("player.change_controller"),loc::Tf("player.change_controller_detail",v.controller),v.canChangeController),
   ConfirmRow("keyboard",loc::T("player.use_keyboard"),loc::T("player.use_keyboard_detail"),v.canChangeController),
   Value("background-play",loc::T("player.background_play"),preferences_.backgroundPlay?loc::T("common.on"):loc::T("common.off"),loc::T("player.background_play_detail"),v.canEditPreferences),
   Row("controls",loc::T("player.native_menus"),loc::T("player.native_menus_detail"),idle)};
 }else if(screen=="interface"){
  title=loc::T("settings.interface_title");char size[32];std::snprintf(size,sizeof(size),"%.2fx",preferences_.interfaceScale);
  const char* hudSizes[]={loc::T("size.small"),loc::T("size.standard"),loc::T("size.large")};
  const auto languageValue=languagePreference_=="auto"?loc::Tf("settings.language.system_with",loc::NativeName(loc::Active())):
   std::string(loc::NativeName(loc::ResolveLocale(languagePreference_,{},{})));
  rows={Value("hud",loc::T("settings.match_hud"),preferences_.showMatchHud?loc::T("common.on"):loc::T("common.off"),reason,v.canEditPreferences),
   Value("hud-layout",loc::T("settings.match_hud_layout"),MatchStripLayoutName(preferences_.matchHudLayout),loc::T("settings.match_hud_layout_detail"),v.canEditPreferences),
   Value("hud-name-offset",loc::T("settings.name_offset"),MatchHudNameOffsetText(preferences_.matchHudNameOffset),loc::T("settings.name_offset_detail"),v.canEditPreferences&&preferences_.matchHudLayout==1),
   Value("hud-size",loc::T("settings.match_hud_size"),hudSizes[(std::max)(0,(std::min)(2,preferences_.matchHudSize))],loc::T("settings.match_hud_size_detail"),v.canEditPreferences),
   Value("hud-position",loc::T("settings.match_hud_position"),MatchStripAnchorName(preferences_.matchHudAnchor),loc::T("settings.match_hud_position_detail"),v.canEditPreferences),
   Value("hud-spacing",loc::T("settings.edge_spacing"),preferences_.matchHudRaised?loc::T("spacing.raised"):loc::T("spacing.normal"),loc::T("settings.edge_spacing_detail"),v.canEditPreferences),
   Value("ready-sound",loc::T("settings.ready_sound"),preferences_.readySound?loc::T("common.on"):loc::T("common.off"),loc::T("settings.ready_sound_detail"),v.canEditPreferences),
   Value("ready-volume",loc::T("settings.ready_sound_volume"),std::to_string(preferences_.readySoundVolume)+"%",loc::T("settings.ready_sound_volume_detail"),v.canEditPreferences&&preferences_.readySound),
   Row("ready-test",loc::T("settings.ready_sound_test"),loc::T("settings.ready_sound_test_detail"),v.canEditPreferences&&preferences_.readySound),
   Value("scale",loc::T("settings.interface_size"),size,reason,v.canEditPreferences),
   Value("language",loc::T("settings.language"),languageValue,languageSaveError_.empty()?std::string(loc::T("settings.language.detail")):languageSaveError_,true)};
  // Select lists the languages by their own names; browsing them changes nothing.
  rows.back().choices=LanguageChoices();rows.back().chosen=languagePreference_;
 }else if(screen=="training-replays"){
  title=loc::T("settings.training_replays_title");
  rows={Value("match-frame-meter",loc::T("settings.match_frame_meter"),preferences_.matchFrameMeter?loc::T("common.on"):loc::T("common.off"),loc::T("settings.match_frame_meter_detail"),v.canEditPreferences),
   Value("training-auto-ready",loc::T("settings.training_auto_ready"),preferences_.trainingAutoReady?loc::T("common.on"):loc::T("common.off"),loc::Tf("settings.training_auto_ready_detail",room::TrainingCall::ReadyWindowMs/1000),v.canEditPreferences),
   Value("replay-save-watched",loc::T("replays.save_watched"),preferences_.recordWatched?loc::T("common.on"):loc::T("common.off"),loc::T("replays.save_watched_detail"),v.canEditPreferences)};
 }else if(screen=="discord"){
  title=loc::T("discord.title");rows={Value("presence",loc::T("discord.show_activity"),preferences_.discordPresence?loc::T("common.on"):loc::T("common.off"),v.discordStatus,v.canEditPreferences),
   Value("invites",loc::T("discord.allow_invitations"),preferences_.discordInvites?loc::T("common.on"):loc::T("common.off"),preferences_.discordPresence?loc::T("discord.invitation_detail"):loc::T("discord.enable_first"),v.canEditPreferences&&preferences_.discordPresence)};
 }else if(screen=="discord-invitation"){
  title=loc::T("discord.invitation_title");rows={Row("invite-cancel",loc::T("discord.cancel_invitation"),loc::T("discord.cancel_invitation_detail"))};
  if(v.discordConfirm)rows.push_back(ConfirmRow("invite-switch",loc::T("discord.switch_room"),v.discordCanSwitch?loc::T("discord.switch_room_detail"):loc::T("discord.wait_game"),v.discordCanSwitch));
  else rows.push_back(Row("invite-wait",loc::T("discord.invitation_pending"),loc::T("discord.invitation_pending_detail"),false));
 }else if(IdentityPanel::Owns(screen)){
  rows=identity_.Rows(v,screen,title);
  if(screen=="tournament-matches"){const auto focus=identity_.TakeFocus(rows);if(!focus.empty())nav.Focus(focus,rows);}
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
 const bool personal=screen=="profile"||screen=="main-character"||screen=="settings"||screen=="player"||screen=="defaults"||screen=="interface"||screen=="training-replays"||screen=="discord";
 std::string status=saveFailed_?loc::T("common.save_failed"):v.settingsPending||preferencesDirty_||saveQueued_||languageDirty_?loc::T("common.saving"):personal?loc::T("common.saved"):"";
 // Severity travels with the status string. This line is the shell's only
 // feedback channel, so a failure must not render like ordinary text.
 Tone statusTone=saveFailed_?Tone::Error:v.settingsPending||preferencesDirty_||saveQueued_||languageDirty_?Tone::Pending:
  personal?Tone::Success:Tone::Neutral;
 if((screen=="create"||screen=="join"||screen=="home"||screen=="online"||screen=="public-rooms")&&opening&&status.empty()){
  status=OpeningCreates(v)?loc::T("room.creating_status"):loc::T("room.joining_status");statusTone=Tone::Pending;
 }
 // Chat keeps the board's line, so a lost connection shows while typing.
 if(screen=="room"&&status.empty()&&v.trainingReadySeconds>0){
  status=loc::Tf("room.training_call.ready_in",v.trainingReadySeconds);statusTone=Tone::Pending;
 }
 if((screen=="room"||screen=="room-chat")&&status.empty()){
  const bool healthy=v.session.control==Health::Healthy;
  status=!healthy?std::string(loc::T("room.reconnecting")):v.room.tournament.Active()?
   loc::Tf("room.tournament_status",static_cast<int>(v.room.tournament.gamesToWin)):
   std::string(loc::T(v.room.serverOwned?(v.room.locked?"room.public_locked_status":"room.public_status"):
    v.room.locked?"room.locked_status":"room.private_status"));
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
  else if(table.phase==room::TablePhase::Ready){
   // A held start counts down on one line; the rows below say who it waits for.
   status=table.spectatorHold?room_controls::HoldStatus(v,table):std::string(loc::T("room.preparing_status"));statusTone=Tone::Pending;
  }
  else if(table.phase==room::TablePhase::Playing&&!finishedGame){
   status=table.resultPending?loc::T("room.waiting_results_status"):loc::T("room.match_in_progress");
   statusTone=Tone::Pending;
  }
  else if(table.phase==room::TablePhase::Closed)status=loc::T("room.table_closed");
  if(v.trainingReadySeconds>0&&v.room.localMember&&seatedLocal){
   status=loc::Tf("room.training_call.ready_in",v.trainingReadySeconds);statusTone=Tone::Pending;
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
 // The failed language save is about the row on the Interface screen, so it
 // speaks there only, and every other report below outranks it.
 if(!languageSaveError_.empty()&&screen=="interface"){status=languageSaveError_;statusTone=Tone::Error;}
 if(IdentityPanel::Owns(screen)){std::string own;Tone ownTone=Tone::Neutral;if(identity_.Status(own,ownTone,ImGui::GetTime())){status=own;statusTone=ownTone;}}
 if(PublicRoomsPanel::Owns(screen)||screen=="create"){std::string own=status;Tone ownTone=statusTone;if(publicRooms_.Status(v,screen,own,ownTone,ImGui::GetTime())){status=own;statusTone=ownTone;}}
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
 if(screen=="replays"&&!v.replayNotice.empty()&&status.empty()){status=v.replayNotice;statusTone=v.replayNoticeError?Tone::Error:Tone::Success;}
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
// The caption an export starts from: the names Ember noted, the date and
// score on one line, Ember's mark, and the set so far. The set is counted
// from the archive: the matches these two played right before this one,
// each within half an hour of the next, by who won each.
void ApplicationShell::OpenReplayExport(const ShellView& view,const ShellView::Replay& replay) {
 exportPath_=replay.path;caption_=replay::Caption{};
 caption_.name[0]=replay.names[0];caption_.name[1]=replay.names[1];
 caption_.names=!replay.names[0].empty()&&!replay.names[1].empty();
 const std::string score=replay.summary?ScoreText(*replay.summary):std::string();
 caption_.text=replay.label.substr(0,10)+(score.empty()?"":"   "+score);
 caption_.line=caption_.mark=true;
 std::uint64_t next=replay.time;
 for(const auto& earlier:view.replays){
  if(!caption_.names||earlier.time>=replay.time)continue;
  if(next-earlier.time>1800)break; // newest first: nothing older continues the set
  const bool same=earlier.names[0]==replay.names[0]&&earlier.names[1]==replay.names[1];
  const bool swapped=earlier.names[0]==replay.names[1]&&earlier.names[1]==replay.names[0];
  if((!same&&!swapped)||!earlier.summary||!earlier.summary->scored||earlier.summary->score[0]==earlier.summary->score[1])continue;
  const int winner=earlier.summary->score[0]>earlier.summary->score[1]?0:1;
  ++caption_.wins[same?winner:1-winner];next=earlier.time;
 }
 caption_.set=caption_.wins[0]+caption_.wins[1]>0;
}
void ApplicationShell::HandleActivate(const MenuAction& a,const ShellView& v,const std::string& screen,bool idle,const Submit& submit) {
 using namespace netplay; auto& nav=menu_.navigation;
 // The retry row a failed save adds to every screen stays the shell's.
 if(IdentityPanel::Owns(screen)&&a.id!="retry-save"){identity_.Activate(a,v,nav);return;}
 if(PublicRoomsPanel::Owns(screen)){
  if(a.id=="pr-quick"){QuickJoin(v);return;}
  // The confirmed setup runs in the Ember ID panel; a locked ID is unlocked on its own screen first.
  if(a.id=="pr-setup"){identity_.SetUpPublicRooms(v);error_.clear();return;}
  if(a.id=="pr-setup-id"){identity_.ClearPublicSetup();nav.Push("identity");return;}
  if(publicRooms_.Activate(a,nav))return;
 }
 // An opening room keeps its own screen, with its Stop row, until it joins.
 if(a.id=="online")nav.Push(idle?"online":v.session.room==RoomState::Opening?OpeningScreen(v):"room");
 else if(a.id=="discord-invitation")nav.Push(a.id);
 else if(a.id=="profile"||a.id=="main-character")nav.Push(a.id);
 else if(a.id.compare(0,5,"main-")==0&&v.canEditPreferences){preferences_.mainFighter=std::stoi(a.id.substr(5));preferencesDirty_=true;profileSavePending_=true;error_.clear();saveAt_=ImGui::GetTime()+.45;}
 else if(a.id=="selection"){selectionFresh_=true;selectionOpenOn_=screen.compare(0,4,"room")==0?"roster":"";nav.Push(a.id);}
 else if(a.id=="replays")nav.Push(a.id);
 else if(a.id=="replay-folder")Service(platform::ServiceAction::OpenReplayFolder,v,submit);
 else if(a.id=="cap-generate"){
  ShellAction r;r.command.generation=v.session.generation;r.replay={replay::Mode::Export,exportPath_,caption_};r.replay.meter=false;
  if(!submit(std::move(r)))error_=loc::T("error.queue_failed");else nav.Return();
 }
 else if(a.id=="replay-log"){ShellAction r;r.command.generation=v.session.generation;r.replay.mode=replay::Mode::OpenLog;if(!submit(std::move(r)))error_=loc::T("error.queue_failed");}
 else if(a.id=="settings"||a.id=="about"||a.id=="create"||a.id=="join"||a.id=="public-rooms"||a.id=="player"||a.id=="defaults"||a.id=="interface"||a.id=="training-replays"||a.id=="discord"||a.id=="identity"||a.id=="developer")nav.Push(a.id);
 else if(a.id=="pr-create"||a.id=="pr-none")OpenPublicCreate();
 else if(a.id=="host"&&preferences_.roomPublic){error_.clear();publicRooms_.Create(preferences_.roomName,preferences_.roomCapacity,preferences_.publicTableRules);}
 else if(a.id=="host"||a.id=="join-now")Send(a.id=="host"?CommandKind::HostRoom:CommandKind::JoinInvite,v,submit);
 else if(a.id=="cancel-open")Send(CommandKind::LeaveRoom,v,submit);
 else if(a.id=="offline"||a.id=="controls")Send(CommandKind::StartOffline,v,submit);
 // Training is the offline menus with the game sent on into Training mode.
 else if(a.id=="training")Send(CommandKind::StartOffline,v,submit,true);
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
 else if(IdentityPanel::Owns(screen))identity_.Accept(a,v);
 else if(a.id=="invite-text")std::snprintf(invitation_,sizeof(invitation_),"%s",a.text.c_str());
 else if(a.id=="language")SetLanguage(std::string(loc::NextPreference(languagePreference_,a.delta)));
 else if(screen=="replay-export"){
  if(a.id=="cap-names")caption_.names=a.delta>0;else if(a.id=="cap-line")caption_.line=a.delta>0;
  else if(a.id=="cap-set")caption_.set=a.delta>0;else if(a.id=="cap-mark")caption_.mark=a.delta>0;
  else if(a.id=="cap-name1")caption_.name[0]=a.text;else if(a.id=="cap-name2")caption_.name[1]=a.text;
  else if(a.id=="cap-text")caption_.text=a.text;
  else if(a.id=="cap-set1"||a.id=="cap-set2"){int& wins=caption_.wins[a.id=="cap-set2"];wins=(std::max)(0,(std::min)(99,wins+a.delta));}
 }
 else{
  auto prior=preferences_;
  if(a.id=="name")preferences_.displayName=a.text;else if(a.id=="room-name")preferences_.roomName=a.text;
  else if(a.id=="capacity")preferences_.roomCapacity=(std::max)(2,(std::min)(16,preferences_.roomCapacity+a.delta));
  else if(a.id=="delay"){
   // Auto sits before the smallest delay. Choosing it keeps the number; leaving it starts at the smallest.
   const int next=StepInputDelay(preferences_.autoInputDelay,preferences_.inputDelay,a.delta);
   preferences_.autoInputDelay=next==AutoInputDelayChoice;if(!preferences_.autoInputDelay)preferences_.inputDelay=next;
  }
  else if(a.id=="hud-layout")preferences_.matchHudLayout=(std::max)(0,(std::min)(1,preferences_.matchHudLayout+a.delta));
  else if(a.id=="hud-name-offset")preferences_.matchHudNameOffset=(std::max)(-netplay::MaxMatchHudNameOffset,(std::min)(netplay::MaxMatchHudNameOffset,preferences_.matchHudNameOffset+2*a.delta));
  else if(a.id=="hud-size")preferences_.matchHudSize=(std::max)(0,(std::min)(2,preferences_.matchHudSize+a.delta));
  else if(a.id=="hud-position")preferences_.matchHudAnchor=(std::max)(0,(std::min)(4,preferences_.matchHudAnchor+a.delta));
  else if(a.id=="hud-spacing")preferences_.matchHudRaised=a.delta>0;
  else if(a.id=="ready-sound")preferences_.readySound=a.delta>0;
  else if(a.id=="training-auto-ready")preferences_.trainingAutoReady=a.delta>0;
  else if(a.id=="match-frame-meter")preferences_.matchFrameMeter=a.delta>0;
  else if(a.id=="background-play")preferences_.backgroundPlay=a.delta>0;
  else if(a.id=="replay-save-watched")preferences_.recordWatched=a.delta>0;
  else if(a.id=="ready-volume")preferences_.readySoundVolume=(std::max)(10,(std::min)(100,preferences_.readySoundVolume+10*a.delta));
  else if(a.id=="scale")preferences_.interfaceScale=(std::max)(1.f,(std::min)(1.5f,preferences_.interfaceScale+.05f*a.delta));
  else if(a.id=="hud")preferences_.showMatchHud=a.delta>0;else if(a.id=="presence")preferences_.discordPresence=a.delta>0;
  else if(a.id=="invites")preferences_.discordInvites=a.delta>0;
  else if(a.id=="visibility"){preferences_.roomPublic=a.delta>0;if(preferences_.roomPublic)ApplyPublicDefaultName();} // UpdatePublicBridge asks the Ember ID for what Public needs
  else AdjustRule(screen=="create"&&preferences_.roomPublic?preferences_.publicTableRules:preferences_.tableRules,a);
  if(!preferences_.Valid()){preferences_=prior;error_=loc::T("error.invalid_value");}
  else{preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;error_.clear();}
 }
}

// The public-room service is the one the Ember ID panel selects when its
// service list arrives. Public rooms, and Create while Visibility is Public
// (restored from the preferences or just chosen), both need it, so this is the
// one place that asks: once per stay in such a state, when no service is
// selected, and not over a request the panel is still waiting on.
void ApplicationShell::UpdatePublicBridge(const ShellView& v,const std::string& screen) {
 const bool needed=PublicRoomsPanel::Owns(screen)||(screen=="create"&&preferences_.roomPublic);
 if(!needed){publicBridgeAsked_=false;return;}
 if(publicBridgeAsked_)return;
 if(identity_.UsableBridge(v).empty()){
  if(identity_.Waiting())return;
  identity_.Probe();
 }
 publicBridgeAsked_=true;
}

// Create public room opens Create on Public, which stays the default until changed.
void ApplicationShell::OpenPublicCreate() {
 if(!preferences_.roomPublic){preferences_.roomPublic=true;preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;}
 ApplyPublicDefaultName();
 error_.clear();menu_.navigation.Push("create");
}
// The room still has the private default name, which would list as "Private room":
// a public room is named for its host instead, within the name's own limit.
void ApplicationShell::ApplyPublicDefaultName() {
 if(preferences_.roomName!=netplay::PlayerPreferences{}.roomName)return;
 const std::size_t overhead=loc::Tf("public.default_name","").size();
 const std::string name=CutUtf8(preferences_.displayName,overhead<64?64-overhead:0);
 if(name.empty())return;
 preferences_.roomName=loc::Tf("public.default_name",name);
 preferencesDirty_=true;saveAt_=ImGui::GetTime()+.45;
}
// Quick join asks for the first room with a free seat, in the player's region when it is known.
// With none it offers to create one, which the next frame opens.
void ApplicationShell::QuickJoin(const ShellView& v) {
 error_.clear();
 if(const auto id=publicRooms_.QuickPick(v,PublicRoomsPanel::KnownRegion(v))){
  if(publicRooms_.Join(*id))menu_.navigation.Prefer("pr-room:"+*id);
  return;
 }
 menu_.ShowNotice(loc::T("public.quick_none"),loc::T("public.quick"),loc::T("public.create_room"),[this]{openPublicCreate_=true;});
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
 Conceal();
 // Not read: the player is not looking at Chat while Ember is hidden, whatever screen it was left on.
 ObserveChat(v,room);
 identity_.Hidden(v,submit,ImGui::GetTime());
}

void ApplicationShell::Draw(const ShellView& v,bool* open,const Submit& submit,const DrawSelection& selection,const DrawSelection& developer) {
 using namespace netplay; auto& nav=menu_.navigation;
 const double now = ImGui::GetTime();
 if(!languageSeeded_){languagePreference_=loc::ValidPreference(v.languagePreference)?v.languagePreference:"auto";languageSeeded_=true;}
 // Called out of Training: the player's own table is shown, where the Ready
 // row carries the time left, and a player who asked for it is readied.
 if(v.trainingCallSequence!=trainingCallSeen_||v.trainingReadySequence!=trainingReadySeen_){
  const auto place=room::PlaceOf(v.room,v.room.localMember);
  const bool seated=v.session.room==RoomState::Joined&&place.kind==room::Place::Kind::Seat&&place.table>=0&&place.table<static_cast<int>(v.room.tables.size());
  if(seated&&v.trainingCallSequence!=trainingCallSeen_)OpenTableOptions(v,place.table);
  trainingCallSeen_=v.trainingCallSequence;
  // The Ready waits for the runtime's gate, and ends with the seat or the window.
  if(!seated||v.trainingReadySeconds<=0)trainingReadySeen_=v.trainingReadySequence;
  else if(v.trainingReadySequence!=trainingReadySeen_&&v.canReady){selectedTable_=place.table;Send(CommandKind::Ready,v,submit);trainingReadySeen_=v.trainingReadySequence;}
 }
 if(lastUiTime_ >= 0 && now < lastUiTime_) {
  // DX9 reset recreates ImGui, but these deadlines belong to the surviving shell.
  // Keep raw-clock users in RoomAction in the same epoch, including queued saves.
  saveAt_ = RebaseUiTimestamp(saveAt_, lastUiTime_, now);
  languageSaveAt_ = RebaseUiTimestamp(languageSaveAt_, lastUiTime_, now);
  noticeUntil_ = RebaseUiTimestamp(noticeUntil_, lastUiTime_, now);
  lockInReleasedUntil_ = RebaseUiTimestamp(lockInReleasedUntil_, lastUiTime_, now);
  shortCopyUntil_ = RebaseUiTimestamp(shortCopyUntil_, lastUiTime_, now);
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
 TrackLockIn(v,now);
 UpdateChat(v);
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
 if(v.discordPending&&inviteRevision_!=v.discordRevision){inviteRevision_=v.discordRevision;nav.Push("discord-invitation");}
 if(!v.discordPending&&nav.Screen()=="discord-invitation")nav.Return();
 // A replay link takes the menu to its question only with no room: in a room the menu is the room's, and the link waits until it is left.
 if(v.replayLink!=replayLinkSeen_&&(v.replayLink.empty()||v.session.room==netplay::RoomState::Idle)){replayLinkSeen_=v.replayLink;if(!v.replayLink.empty()&&nav.Screen()!="replays"){nav.Home();nav.Push("replays");}}
 if(v.inputCapture!=input::Capture::Idle&&nav.Screen()!="assignment")nav.Push("assignment");
 if(v.inputCapture==input::Capture::Idle&&nav.Screen()=="assignment")nav.Return();
 identity_.Update(v,nav.Screen(),submit,now);
 publicRooms_.Update(v,nav.Screen(),identity_.UsableBridge(v),identity_.Waiting(),submit,now);
 UpdatePublicBridge(v,nav.Screen());
 {std::string said;if(publicRooms_.TakeSaid(said)){notice_=said;noticeTone_=Tone::Error;noticeUntil_=now+6;}}
 // The setup finished while the player waited or looked away: say so once, and the list is already on its way.
 if(identity_.PublicSetup().step==PublicSetupStep::Done){identity_.ClearPublicSetup();notice_=loc::T("public.setup_done");noticeTone_=Tone::Success;noticeUntil_=now+4;}
 // A tournament match that ended is announced wherever the player is: the
 // room it was played in closes with it.
 if(v.tournament.phase!=tournamentPhase_){
  using netplay::tournament::Phase;
  if(v.tournament.phase==Phase::Finished){notice_=loc::T("tournament.done_notice");noticeTone_=Tone::Success;noticeUntil_=now+12;}
  else if(v.tournament.phase==Phase::Failed){
   notice_=loc::Tf("tournament.stopped_notice",IdentityPanel::TournamentFailure(v.tournament.reason));noticeTone_=Tone::Error;noticeUntil_=now+20;
  }
  tournamentPhase_=v.tournament.phase;
 }
 // A match link from the browser opens the match's row for the player to
 // press Play. It never leaves a room by itself: in one, it only says so.
 if(v.tournament.link.sequence!=matchLinkSequence_){
  matchLinkSequence_=v.tournament.link.sequence;
  identity_.OpenMatch(v.tournament.link.bridge,v.tournament.link.match);
  if(v.session.room==RoomState::Idle){
   if(nav.Screen()!="tournament-matches"){nav.Home();nav.Push("identity");nav.Push("tournament-matches");}
   notice_=loc::T("tournament.link_opened");noticeTone_=Tone::Success;
  }else{notice_=loc::T("tournament.link_waiting");noticeTone_=Tone::Pending;}
  noticeUntil_=now+15;
 }
 // A match newly assigned is announced outside a room, so the player never
 // has to go looking for it.
 if(v.session.room==RoomState::Idle&&identity_.TakeAssigned(v)){
  notice_=loc::T("tournament.assigned_notice");noticeTone_=Tone::Success;noticeUntil_=now+15;
 }
 // A Discord connect link opens Connect Discord for its service, where the
 // player decides. In a room it waits until the room closes.
 if(v.tournament.connect.sequence!=connectLinkSequence_){
  connectLinkSequence_=v.tournament.connect.sequence;pendingConnect_=v.tournament.connect.bridge;
  // In a room the link waits, and so is no longer fresh when it opens.
  if(v.session.room!=RoomState::Idle)pendingConnectStale_=true;
  if(v.session.room!=RoomState::Idle){notice_=loc::T("connect.link_waiting");noticeTone_=Tone::Pending;noticeUntil_=now+15;}
 }
 if(!pendingConnect_.empty()&&v.session.room==RoomState::Idle){
  // A link the player just clicked goes on by itself; one that arrived
  // during play or waited for a room asks first.
  const bool fresh=!pendingConnectStale_&&!v.tournament.connect.confirm;
  identity_.OpenDiscord(pendingConnect_,fresh);pendingConnect_.clear();pendingConnectStale_=false;
  if(nav.Screen()!="discord-connect"){nav.Home();nav.Push("identity");nav.Push("discord-connect");}
 }
 const auto* vp=ImGui::GetMainViewport();ImGui::SetNextWindowPos(vp->Pos);ImGui::SetNextWindowSize(vp->Size);
 ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(20*Scale(),16*Scale()));ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,0);
 ImGui::Begin("SF4 Ember Netplay###EmberShell",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoNavInputs);
 // Room shortcuts work from every screen of a joined room, including the
 // embedded fighter selector, which forwards what it does not handle.
 const bool inRoom=v.session.room!=RoomState::Idle&&v.room.roomEpoch;
 const bool keys=KeyboardPrompts();
 std::vector<LegendHint> roomHints=keys?std::vector<LegendHint>{{"F",loc::T("room.legend_fighter")},{"T",loc::T("room.legend_options")},{"C",loc::T("room.chat")}}:
  std::vector<LegendHint>{{"X",loc::T("room.legend_fighter")},{"Y",loc::T("room.legend_options")},{"View",loc::T("room.chat")}};
 if(nav.Screen()=="selection"&&selection){
  // The selector names where its Back goes and shows the room's shortcuts it hands back.
  SetEmbeddedReturn({MenuScreenLabel(nav.Parent()),inRoom?roomHints:std::vector<LegendHint>{},selectionFresh_,selectionOpenOn_});
  selectionFresh_=false;selectionOpenOn_.clear();
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
 // Public rooms lead with a toolbar of four cells, in one row on a wide list and two on a narrow one.
 const bool publicToolbar=PublicRoomsPanel::Owns(screen)&&!rows.empty()&&rows[0].id=="pr-quick";
 // The main-character grid follows USFIV's select screen, as fighter select does.
 const RosterGrid roster=LayOutRosterGrid(ImGui::GetContentRegionAvail().x);
 const int columns=screen=="main-character"?roster.columns:
  publicToolbar?(PublicListWidth()>=600*Scale()?4:2):1;
 GameMenu::Card portraits;
 // The public rooms' toolbar, room cards and state cards are drawn by PublicRoomsCards; the rest stay plain rows.
 // The cards get the setup and each card's phase from the panels that own them, not from the rows.
 const PublicSetupView publicSetup=PublicRoomsPanel::Owns(screen)?identity_.PublicSetup():PublicSetupView{};
 menu_.cardRounding=0;menu_.compactDetailLines=0;menu_.wideListShare=screen=="main-character"?roster.listShare:.53f;
 if(PublicRoomsPanel::Owns(screen)){
  menu_.cardRounding=PublicCardRounding;
  // With the toolbar and the cards to show, a narrow layout gives them the screen, not the detail text.
  menu_.compactDetailLines=publicToolbar?2:0;
  portraits=[&](const MenuEntry& e,ImVec2 min,ImVec2 max){
   return DrawPublicCard(v,publicRooms_,publicSetup,publicRooms_.PhaseOf(e.id),e,min,max,e.id==menu_.navigation.Focus(),now);};
 }
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
 if(PublicRoomsPanel::Owns(screen))profilePreview=[&](const std::string& id){DrawPublicPreview(v,publicRooms_,id);};
 GameMenu::Body board;
 if(screen=="interface")profilePreview=[&](const std::string&){
  ImGui::TextUnformatted(loc::T("settings.preview"));
  MatchStripView preview;preview.names[0]=loc::T("settings.player_one");preview.names[1]=loc::T("settings.player_two");
  preview.pingMs=68;preview.rollbackFrames=2;preview.appliedDelay=3;
  preview.size=preferences_.matchHudSize;preview.raised=preferences_.matchHudRaised;preview.anchor=preferences_.matchHudAnchor;preview.layout=preferences_.matchHudLayout;preview.nameOffset=preferences_.matchHudNameOffset;
  preview.spectators=2;
  DrawMatchStripPreview(preview);
 };
 if(screen=="room"&&v.room.roomEpoch)board=[&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction& action,float height,const MenuVisualFeedback& feedback){DrawRoomBoard(v,entries,navigation,action,height,feedback);};
 if(screen=="room-chat"&&v.room.roomEpoch)board=[&](const std::vector<MenuEntry>& entries,MenuNavigation& navigation,MenuAction& action,float height,const MenuVisualFeedback& feedback){DrawChatScreen(v,entries,navigation,action,height,feedback);};
 // Visual grace cannot grant permission: enabled and all dispatch checks stay live.
 const bool checkpointPending=roomScreen && RoomCheckpointPending(v) && !v.controllerUnavailable &&
  SessionProblem(v.session).empty() && v.error.empty() && error_.empty();
 for(auto& row:rows)row.pending=checkpointPending;
 // Every shell screen except Home reserves the status area, so a status that
 // grows can never displace the list under a highlight or a mouse click.
 // Home renders its status in the small-print line below the list instead.
 const bool stableFeedback=screen!="home";
 // With the message box holding the keyboard, F, T and C type letters, so the keys are not offered there.
 if(roomScreen&&v.room.roomEpoch)menu_.shortcutHints=screen=="room-chat"&&keys&&RoomActionsAvailable(v)?std::vector<LegendHint>{}:roomHints;
 else if(PublicRoomsPanel::Owns(screen)&&publicRooms_.Refreshable(v))menu_.shortcutHints={{keys?"T":"Y",loc::T("legend.refresh")}};
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
 // An opening public room's card is information, so the cursor waits on its Stop row.
 if(PublicRoomsPanel::Owns(screen)&&opening&&nav.Focus()=="pr-opening")nav.Focus("cancel-open",rows);
 auto a=menu_.Draw(title.c_str(),rows,status.c_str(),profilePreview,columns,portraits,board,0,publicToolbar?46:screen=="main-character"?roster.cardHeight:100,stableFeedback,statusTone,screen=="home");
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
  else if(PublicRoomsPanel::Owns(screen))publicRooms_.Shortcut(a,v);
 }else if(a.kind==MenuAction::Chosen){
  if(roomScreen)RoomAction(a,v,submit);
  else if(IdentityPanel::Owns(screen))identity_.Accept(a,v);
  else if(PublicRoomsPanel::Owns(screen))publicRooms_.Choose(a);
  else if(a.id=="language")SetLanguage(a.text);
  else if(a.id=="replay-link"){
   ShellAction r;r.command.generation=v.session.generation;
   if(a.text=="link-play")r.replay={replay::Mode::Watch,v.replayLink};else r.replay.mode=replay::Mode::DismissLink;
   if(!submit(std::move(r)))error_=loc::T("error.queue_failed");
  }
  else if(a.id.compare(0,7,"replay:")==0&&a.text=="inputs"){
   // Nothing is sent to the game: the screen names the file and is handed its match (ReplayInputsFile).
   for(const auto& shown:v.replays)if(shown.path==a.id.substr(7)){inputsFile_=shown.path;++inputsRevision_;menu_.navigation.Push("replay-inputs");break;}
  }
  else if(a.id.compare(0,7,"replay:")==0&&a.text=="export"){
   // The caption is set up first; Generate on that screen sends the export.
   for(const auto& shown:v.replays)if(shown.path==a.id.substr(7)){OpenReplayExport(v,shown);menu_.navigation.Push("replay-export");break;}
  }
  else if(a.id.compare(0,7,"replay:")==0&&v.replaysReady){
   ShellAction r;r.command.generation=v.session.generation;r.replay={(a.text=="watch"||a.text=="watch-meter")?replay::Mode::Watch:replay::Mode::Add,a.id.substr(7)};r.replay.meter=a.text=="watch-meter";
   if(!submit(std::move(r)))error_=loc::T("error.queue_failed");
  }
 }else if(a.kind==MenuAction::Activate){
  HandleActivate(a,v,screen,idle,submit);
 }else if(a.kind==MenuAction::Adjust||a.kind==MenuAction::TextAccepted){
  HandleAdjust(a,v,screen,submit);
 }
 // Accepted text may be a passphrase; the panel kept its own copy.
 if(a.kind==MenuAction::TextAccepted)WipeText(a.text);
 ImGui::End();ImGui::PopStyleVar(2);
}
} }
