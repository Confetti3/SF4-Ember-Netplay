#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include "../session/TrainingCall.hxx"
#include "../training/PracticeSettings.hxx"
#include "../training/RecordingFile.hxx"
#include "../netplay/RoomPreferences.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/TrainingInRoom.hxx"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

// The Training settings have their own screen. Offline, Training is the
// game's own mode under Play offline: Home has no Training row, and Play
// offline sends the game nowhere further.
void TrainingSettingsAndHome() {
 using namespace sf4e;
 using Kind=netplay::CommandKind;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 h.Choose("settings");
 const std::vector<std::string> settings={"player","country","defaults","interface","training-replays","discord","problem-reports","about"};
 Check(rows.size()==settings.size(),"Settings row count changed");
 for(std::size_t i=0;i<settings.size();++i)Check(rows[i].id==settings[i],"Settings order changed");
 h.Choose("interface");
 const std::vector<std::string> interfaceRows={"hud","hud-layout","hud-name-offset","hud-size","hud-position","hud-spacing","ready-sound","ready-volume","ready-test","scale","language"};
 Check(rows.size()==interfaceRows.size(),"Interface no longer has Stable's 11 rows");
 for(std::size_t i=0;i<interfaceRows.size();++i)Check(rows[i].id==interfaceRows[i],"Interface order changed");
 h.Press(MenuInput::Back);h.Choose("training-replays");
 Check(rows.size()==4&&rows[0].id=="match-frame-meter"&&rows[1].id=="training-auto-ready"&&rows[2].id=="replay-save-watched"&&rows[3].id=="watched-picture","Training and replays settings order wrong");
 Check(rows[1].detail==loc::Tf("settings.training_auto_ready_detail",room::TrainingCall::ReadyWindowMs/1000),"Auto-ready detail lost runtime deadline");
 h.Choose("match-frame-meter");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.matchFrameMeter,"Match frame meter did not save from new screen");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("training-auto-ready");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.trainingAutoReady,"Training auto-ready did not save from new screen");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 // Country opens its picker on the saved choice; a country saves and goes back to Settings.
 h.Screen("settings");h.Choose("country");
 Check(h.shell.Navigation().Screen()=="country"&&h.shell.Navigation().Focus()=="country-auto","The country picker did not open on Automatic");
 h.shell.Navigation().Focus("country-JP",rows);h.Press(MenuInput::Select);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.country==CountryPreference::Of(CountryCode::Parsed("JP")),"A picked country did not save");
 Check(h.shell.Navigation().Screen()=="settings","Choosing a country did not go back to Settings");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 h.Screen("home");
 Check(std::none_of(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="training";}),"Home still has its own Training row");
 SetMenuEntriesProbe({});h.Choose("offline");
 Check(!h.actions.empty()&&h.actions.back().command.kind==Kind::StartOffline&&h.actions.back().training==TrainingEntry::None,"Play offline asked for Training");
 // Private, then public, then private again, with the meter saved On: the choice survives the public room,
 // entering it saves nothing, and the row says why it is not offered only there.
 h.view.preferences.matchFrameMeter=true;
 const auto saves=[&]{return std::count_if(h.actions.begin(),h.actions.end(),[](const ShellAction& a){return a.command.kind==Kind::SavePreferences;});};
 const auto savedBefore=saves();
 for(const bool publicRoom:{false,true,false}){
  JoinedRoom(h,9);h.view.room.serverOwned=publicRoom;h.view.canEditPreferences=false;
  std::vector<MenuEntry> shown;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){shown=entries;});
  h.Frame();h.Screen("training-replays");
  Check(!shown.empty()&&shown[0].id=="match-frame-meter"&&!shown[0].enabled,"Frame meter row changed in a room");
  Check(shown[0].value==loc::T("common.on")&&h.view.preferences.matchFrameMeter,"A public room lost the saved frame meter choice");
  Check(shown[0].detail==loc::T(publicRoom?"settings.match_frame_meter_private":"settings.match_frame_meter_detail"),"Frame meter detail ignores the room kind");
  Check(saves()==savedBefore,"Entering a room saved preferences");
 }
 SetMenuEntriesProbe({});
}
// Training from inside a room: the row sends no room command, and a player
// called back from Training lands on their table, readied for them only once
// the runtime allows a Ready.
void TrainingFromRoom() {
 using namespace sf4e;
 using Kind=netplay::CommandKind;
 Harness h;h.Frame();
 room::Member me,other;me.id=1;me.name="Me";other.id=2;other.name="Other";
 JoinedRoom(h,9);h.view.session.control=netplay::Health::Healthy;
 h.view.room.members={me,other};h.view.canTrain=true;h.Frame();
 std::vector<MenuEntry> rows;std::string status;Tone tone=Tone::Neutral;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 SetMenuStatusProbe([&](const char* text,Tone value){status=text;tone=value;});h.Frame();
 const auto training=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="room-training";});
 Check(training!=rows.end()&&training->label==loc::T("room.training")&&std::next(training)->id=="leave","Wait in Training label or position wrong");
 // The room's status line says it is a Practice room.
 Check(status==loc::T("room.private_status"),"A private room's status changed");
 h.view.room.practice=true;h.Frame();
 Check(status==loc::T("room.practice_status"),"A Practice room's status did not say so");
 h.view.room.locked=true;h.Frame();
 Check(status==loc::T("room.practice_locked_status"),"A locked Practice room's status did not say so");
 h.view.room.practice=h.view.room.locked=false;h.Frame();
 const auto before=h.actions.size();
 h.Choose("room-training");
 Check(h.actions.size()==before+1&&h.actions.back().training==TrainingEntry::Room&&h.actions.back().command.kind!=Kind::StartOffline,"Training from the room did not ask for Training alone");
 // Seated opposite another fighter and called back: the table page, with the time to ready on its Ready row.
 me.table=0;me.seat=0;me.status=room::MemberStatus::Seated;other.table=0;other.seat=1;other.status=room::MemberStatus::Seated;
 h.view.room.members={me,other};h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;h.view.room.tables[0].phase=room::TablePhase::Waiting;
 h.view.canTrain=false;h.view.trainingCallSequence=1;h.view.trainingReadySeconds=15;h.Frame();
 Check(h.shell.Navigation().Screen()=="room-table","A player called back from Training was not shown their table");
 Check(status==loc::Tf("room.training_call.ready_in",15)&&tone==Tone::Pending,"Training deadline missing from table status");
 h.Screen("room");Check(status==loc::Tf("room.training_call.ready_in",15)&&tone==Tone::Pending,"Training deadline missing from room status");h.Screen("room-table");
 // In a Practice room every game takes the longest round time, whatever the table's rules say.
 h.view.room.practice=true;h.Frame();
 const auto rules=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="rules";});
 Check(rules!=rows.end()&&rules->value==loc::Tf("room.rules_summary_practice",3,room::PracticeRoundTime,loc::T("common.on")),"Non-host cannot see the Practice room's rules");
 // The host edits the other rules there, but has no round time to choose.
 const auto hasRow=[&](const char* id){return std::any_of(rows.begin(),rows.end(),[&](const MenuEntry& row){return row.id==id;});};
 h.view.room.host=1;h.Frame();
 Check(hasRow("rounds")&&!hasRow("time"),"A Practice room's host was offered a round time");
 h.view.room.practice=false;h.Frame();
 Check(hasRow("rounds")&&hasRow("time"),"A plain room's host lost the round time");
 h.view.room.host=0;h.view.room.practice=true;h.Frame();
 // Asked to be readied: nothing until the runtime's gate opens, then one Ready.
 const auto sent=h.actions.size();
 h.view.trainingReadySequence=1;h.Frame();h.Frame();
 Check(h.actions.size()==sent,"A Ready was sent before the runtime allowed one");
 h.view.canReady=true;h.Frame();
 Check(h.actions.size()==sent+1&&h.actions.back().command.kind==Kind::Ready,"A player who asked for it was not readied after the call");
 h.Frame();h.Frame();
 Check(h.actions.size()==sent+1,"The call readied the player more than once");
 // A window that closed takes a waiting Ready with it.
 h.view.canReady=false;h.view.trainingReadySequence=2;h.Frame();h.view.trainingReadySeconds=0;h.Frame();h.view.canReady=true;h.Frame();
 Check(h.actions.size()==sent+1,"A Ready outlived its window");
 // A refused submit does not use up the request: it is tried again, and lands exactly once.
 h.view.trainingReadySeconds=15;h.view.trainingReadySequence=3;h.accept=false;h.Frame();
 const auto readies=[&]{return std::count_if(h.actions.begin()+sent,h.actions.end(),[](const ShellAction& a){return a.command.kind==Kind::Ready;});};
 Check(readies()==2,"A refused Ready was not attempted");
 h.accept=true;h.Frame();
 Check(readies()==3,"A refused Ready was not retried once the queue took it");
 h.Frame();h.Frame();
 Check(readies()==3,"A retried Ready was sent more than once");
 // A refusal does not outlast the window either.
 h.view.trainingReadySequence=4;h.accept=false;h.Frame();h.view.trainingReadySeconds=0;h.Frame();h.accept=true;h.Frame();h.Frame();
 Check(readies()==4,"A refused Ready kept retrying after its window closed");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// Waiting in Training from a room: the line the HUD shows about the room, who
// the call's banner names, go now's press and glyph, and the round time a
// Practice match takes.
void TrainingInRoom() {
 using namespace sf4e;using loc::T;using loc::Tf;
 using Line=std::vector<std::string>;
 room::Snapshot s;s.roomEpoch=5;s.name="Friday Night";s.localMember=1;
 for(std::size_t i=0;i<s.tables.size();++i)s.tables[i].id=static_cast<std::uint8_t>(i);
 room::Member me,alex,sam;me.id=1;me.name="Me";alex.id=2;alex.name="Alex";alex.fighter=0;sam.id=3;sam.name="Sam";
 s.members={me,alex,sam};
 // The line as drawn with both names whole: the room, then Ember's words,
 // with the opponent's name kept apart so only it and the room's give way.
 const auto line=[&](unsigned unread){const auto status=ui::DescribeTrainingRoom(s,unread);
  Line all;const auto add=[&](const std::string& part){if(!part.empty())all.push_back(part);};
  add(status.room);add(status.table);add(status.opponent.empty()?status.place:Tf("training.room.opponent",status.opponent));add(status.unread);
  Check(ui::TrainingRoomLine(status,status.room,status.opponent)==[&]{std::string joined;for(const auto& part:all)joined+=(joined.empty()?"":"  \xC2\xB7  ")+part;return joined;}(),
   "The room line's parts were not joined in order");
  return all;};
 Check(ui::DescribeTrainingRoom(room::Snapshot{},2).Empty(),"Training showed a room line with no room");
 Check(line(0)==Line({"Friday Night",T("training.room.not_queued")}),"A member with no place was not told so");
 // Second of three in table 2's queue, with chat waiting.
 s.tables[1].queue={3,1,2};
 Check(line(3)==Line({"Friday Night",Tf("training.room.table",2),Tf("training.room.queued",2,3),Tf("training.room.unread",3u)}),
  "A queued member's line did not give the table, the place in the queue and the unread chat");
 s.tables[1].queue={1};
 Check(line(0)==Line({"Friday Night",Tf("training.room.table",2),Tf("training.room.queued",1,1)}),"The first in the queue was not told so, or unread 0 was shown");
 // Seated alone, then with someone opposite.
 s.tables[1].queue.clear();s.tables[1].p1=1;s.members[0].table=1;s.members[0].seat=0;
 Check(line(0)==Line({"Friday Night",Tf("training.room.table",2),T("training.room.waiting")}),"A seated member alone was not told they wait for an opponent");
 input::CallIdentity called;called.roomEpoch=5;called.table=1;called.opponent=2;called.generation=12;
 Check(!ui::DescribeChallenger(s,input::CallIdentity{}).called,"The banner showed with no call");
 s.tables[1].p2=2;s.members[1].table=1;s.members[1].seat=1;
 Check(line(1)==Line({"Friday Night",Tf("training.room.table",2),Tf("training.room.opponent","Alex"),Tf("training.room.unread",1u)}),
  "A seated member did not see who sat down opposite");
 Check(ui::DescribeTrainingRoom(s,0).opponent=="Alex"&&ui::DescribeTrainingRoom(s,0).place.empty(),"The opponent's name was not kept apart");
 const auto call=ui::DescribeChallenger(s,called);
 Check(call.called&&call.opponent=="Alex"&&call.fighter==0&&!call.goNowGlyph,"The banner did not name the opponent and their fighter");
 // The call is who called, not whoever sits there now: Sam taking Alex's
 // seat is a new call, and the old one names Alex or nobody, never Sam.
 s.tables[1].p2=3;s.members[2].table=1;s.members[2].seat=1;
 Check(ui::DescribeChallenger(s,called).opponent=="Alex","The banner named the member who sits there now, not the one who called");
 called.roomEpoch=6;
 Check(!ui::DescribeChallenger(s,called).called,"A call from another room showed its banner");
 called.roomEpoch=5;s.tables[1].p2=2;
 s.localMember=9;
 Check(ui::DescribeTrainingRoom(s,1).Empty(),"A client no longer in the room saw its line");
 // Go now's prompt names the device the training HUD's prompts show: View
 // for an Xbox pad, Enter for the keyboard and for a DirectInput pad, whose
 // buttons have no prompt art. The pad's press itself is the training pad
 // gesture's (ControllerNavigationTest: TrainingPadChord).
 Check(std::string(ui::GoNowGlyph(input::PadXInput))=="View","An Xbox pad was not offered View");
 Check(std::string(ui::GoNowGlyph(input::PadDirectInput))=="Enter","A DirectInput pad was offered a pad button");
 Check(std::string(ui::GoNowGlyph(input::PadKeyboard))=="Enter","A keyboard player was offered a pad button");
}
// A Practice room is chosen on the private Create screen only: off whenever
// Create opens, never saved with the defaults, carried by Create room, and
// with no round time to choose.
void PracticeRoomCreate() {
 using namespace sf4e;
 using Kind=netplay::CommandKind;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& r){return r.id==id;});};
 h.Screen("online");h.Choose("create");
 Check(h.shell.Navigation().Screen()=="create"&&row("practice-room")!=rows.end()&&row("practice-room")->value==loc::T("common.off")&&
  row("time")!=rows.end(),"Create did not open with Practice room off");
 Check(row("practice-room")->detail==loc::Tf("room.practice.detail",room::PracticeRoundTime),"Practice room did not explain itself");
 h.FocusOn("practice-room");h.Press(MenuInput::Right);
 Check(row("practice-room")->value==loc::T("common.on")&&row("time")==rows.end(),"Practice room did not turn on, or left a round time to choose");
 h.Choose("host");
 Check(!h.actions.empty()&&h.actions.back().command.kind==Kind::HostRoom&&h.actions.back().practiceRoom,"Create room did not ask for a Practice room");
 const auto saved=netplay::RoomPreferences(h.actions.back().preferences);
 Check(!saved.contains("practice")&&!saved.contains("training"),"The Practice room choice reached the saved defaults");
 h.Screen("online");h.Choose("create");
 Check(row("practice-room")->value==loc::T("common.off"),"Practice room was still on when Create opened again");
 h.Choose("host");
 Check(h.actions.back().command.kind==Kind::HostRoom&&!h.actions.back().practiceRoom,"A room created with Practice room off asked for one");
 // A public room is never one, and the gameplay defaults do not offer it.
 h.FocusOn("visibility");h.Press(MenuInput::Right);
 Check(row("practice-room")==rows.end()&&row("time")!=rows.end(),"Public Create offered Practice room");
 h.Press(MenuInput::Left);
 h.Screen("defaults");
 Check(row("practice-room")==rows.end()&&row("time")!=rows.end(),"Gameplay defaults offered Practice room");
 SetMenuEntriesProbe({});
}
// Create offers the room's delay policy: Same for both to begin with, kept with
// the defaults once changed, carried by Create room, and offered to a public
// room and on the gameplay defaults too.
void DelayPolicyCreate() {
 using namespace sf4e;
 using Kind=netplay::CommandKind;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& r){return r.id==id;});};
 h.Screen("online");h.Choose("create");
 Check(h.shell.Navigation().Screen()=="create"&&row("delay-policy")!=rows.end()&&row("delay-policy")->value==loc::T("room.delay_policy.shared"),
  "Create did not open with the delay the same for both");
 Check(row("delay-policy")->label==loc::T("room.delay_policy")&&row("delay-policy")->detail==loc::T("room.delay_policy.detail"),"The delay policy did not explain itself");
 h.Choose("host");
 Check(!h.actions.empty()&&h.actions.back().command.kind==Kind::HostRoom&&h.actions.back().delayPolicy==room::DelayPolicy::Shared,"Create room did not ask for a Shared delay");
 Check(!netplay::RoomPreferences(h.actions.back().preferences).contains("delay"),"A Shared delay was written to the saved defaults");
 h.Screen("online");h.Choose("create");
 h.FocusOn("delay-policy");h.Press(MenuInput::Right);
 Check(row("delay-policy")->value==loc::T("room.delay_policy.own"),"The delay policy did not turn to each player's own");
 h.Choose("host");
 Check(h.actions.back().command.kind==Kind::HostRoom&&h.actions.back().delayPolicy==room::DelayPolicy::Own,"Create room did not ask for each player's own delay");
 Check(netplay::RoomPreferences(h.actions.back().preferences).at("delay")=="own","The delay policy did not reach the saved defaults");
 // Kept with the defaults: it is still chosen when Create opens again, and a Practice room has it too.
 h.Screen("online");h.Choose("create");
 Check(row("delay-policy")->value==loc::T("room.delay_policy.own"),"The delay policy was forgotten when Create opened again");
 h.FocusOn("practice-room");h.Press(MenuInput::Right);
 Check(row("delay-policy")!=rows.end(),"A Practice room's Create lost the delay policy");
 h.Press(MenuInput::Left);
 // A public room and the gameplay defaults offer it as well.
 h.FocusOn("visibility");h.Press(MenuInput::Right);
 Check(row("delay-policy")!=rows.end()&&row("practice-room")==rows.end(),"Public Create did not offer the delay policy");
 h.Press(MenuInput::Left);
 h.FocusOn("delay-policy");h.Press(MenuInput::Left);
 Check(row("delay-policy")->value==loc::T("room.delay_policy.shared"),"The delay policy did not turn back");
 h.Choose("host");
 Check(h.actions.back().command.kind==Kind::HostRoom&&h.actions.back().delayPolicy==room::DelayPolicy::Shared,"Create room did not ask for a Shared delay again");
 h.Screen("defaults");
 Check(row("delay-policy")!=rows.end(),"Gameplay defaults did not offer the delay policy");
 SetMenuEntriesProbe({});
}
// An Own room says so on its table page and in its rules, and the delay row
// shows the player's own delay instead of a match value.
void DelayPolicyInRoom() {
 using namespace sf4e;
 Harness h;h.Frame();
 room::Member me,other;me.id=1;me.name="Me";other.id=2;other.name="Other";
 JoinedRoom(h,9);h.view.session.control=netplay::Health::Healthy;
 me.table=0;me.seat=0;me.status=room::MemberStatus::Seated;other.table=0;other.seat=1;other.status=room::MemberStatus::Seated;
 h.view.room.members={me,other};h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;h.view.room.tables[0].phase=room::TablePhase::Waiting;
 h.view.room.host=0;h.view.opponentDelay=4;h.Frame();
 std::vector<MenuEntry> rows;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 h.Screen("room-table");
 const auto row=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& r){return r.id==id;});};
 const auto& rules=h.view.room.tables[0].rules;
 const auto shared=loc::Tf("room.rules_summary",static_cast<int>(rules.roundCount),static_cast<int>(rules.roundTime),loc::T(rules.editionSelect?"common.on":"common.off"));
 Check(row("rules")!=rows.end()&&row("rules")->value==shared,"A Shared room's rules summary changed");
 Check(row("input-delay")!=rows.end()&&row("input-delay")->detail.find(loc::Tf("room.input_delay.match",""))!=std::string::npos&&
  row("input-delay")->detail.find(loc::Tf("room.input_delay.own",""))==std::string::npos,"A Shared room's delay row lost its match value");
 h.view.room.delayPolicy=room::DelayPolicy::Own;h.Frame();
 Check(row("rules")->value==loc::Tf("room.rules_summary_own_delay",shared),"An Own room's rules summary did not say so");
 Check(row("input-delay")->detail.find(loc::Tf("room.input_delay.own",""))!=std::string::npos&&
  row("input-delay")->detail.find(loc::Tf("room.input_delay.match",""))==std::string::npos,"An Own room's delay row showed a match value");
 // The host edits the rules in place, so an Own room is named by its own read-only row.
 Check(row("delay-policy")==rows.end(),"A player who is not the host was shown the host's delay row");
 h.view.room.host=1;h.Frame();
 Check(row("delay-policy")!=rows.end()&&row("delay-policy")->value==loc::T("room.delay_policy.own"),"An Own room's host was not told so");
 h.view.room.delayPolicy=room::DelayPolicy::Shared;h.Frame();
 Check(row("delay-policy")==rows.end(),"A Shared room's host was shown the delay row");
 SetMenuEntriesProbe({});
}
void TrainingJourneys() {
 using namespace sf4e;
 HeadlessImGui imgui;auto& io=imgui.io;
 training::View v;v.available=v.ready=v.checkpoint=true;v.generation=77;v.lengths[0]=20;v.on[0]=true;v.playable=v.replyable=1;
 std::vector<training::Command> commands;
 training::DummyPlan runtimePlan;
 bool queueOpen=true;
 const auto submit=[&](training::Command c){
  if(!queueOpen)return false;
  if(c.action==training::Action::DummyPlan){
   Check(training::ValidDummyPlan(c.plan),"Reply command failed the runtime validator");runtimePlan=c.plan;
  }
  commands.push_back(c);return true;
 };
 // A saved invalid reply remains an error and never submits an empty plan
 // that would select the recording slot. Also exercise the key decoder at
 // its real loader: Reset F11 must not leave Save on its F11 default.
 const auto folder=std::filesystem::temp_directory_path()/("sf4e-shell-training-"+
  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
 std::filesystem::create_directories(folder);
 {std::ofstream file(folder/"training.json");file<<R"({"reply":{"when":4,"moves":"not a move"},"keys":{"reset_position":10}})";}
 SetTrainingDirectory(folder.wstring());
 std::filesystem::create_directory(folder/"recordings");
 for(const char* name:{"First","Second"}){std::ofstream file(folder/"recordings"/(std::string(name)+".json"));
  file<<training::ExportRecording(std::vector<training::Input>(name[0]=='F'?3:7));}
 ImGui::NewFrame();
 TrainingHotkeys(v,submit);
 bool failed=false;const auto notice=TrainingNotice(failed);
 Check(commands.empty()&&failed&&!notice.empty(),"Invalid saved reply silently selected the recording slot");
 Check(TrainingHotkeyBound(10)&&TrainingHotkeyBound(1),"Loaded colliding position keys did not resolve the fallback pair");
 ImGui::Render();
 // Keys only: the pad's own journey is below.
 auto frame=[&](unsigned held=0){io.DeltaTime=1.f/60;SetMenuInput({0,0});
  const ImGuiKey keys[]={ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_Enter,ImGuiKey_Escape};
  for(unsigned i=0;i<6;++i)io.AddKeyEvent(keys[i],(held&(1u<<i))!=0);
  ImGui::NewFrame();
  DrawTrainingFlyout(v,submit);ImGui::Render();};
 auto press=[&](unsigned held){frame();frame(held);frame();};
 auto reach=[&](const char* id){frame();for(int i=0;i<100&&TrainingNavigation().Focus()!=id;++i)press(MenuInput::Up);
  for(int i=0;i<100&&TrainingNavigation().Focus()!=id;++i)press(MenuInput::Down);
  Check(TrainingNavigation().Focus()==id,"Training entry unreachable");};
 auto choose=[&](const char* id){reach(id);press(MenuInput::Select);};
 frame();
 Check(TrainingNavigation().Focus()=="recording","Removed save-position section is still in training navigation");
 press(MenuInput::Down);
 Check(TrainingNavigation().Focus()=="history","Removed frame panel is still in training navigation");
 // The pad drives the controls as the keys do: a press moves the highlight
 // and turns the prompts to the pad's, Select opens a page, Back returns, and
 // Back at the root closes the controls.
 {
  SetMenuGlyphs(3,0x40000,0x20000);
  const auto padFrame=[&](unsigned held){io.DeltaTime=1.f/60;SetMenuInput({held,0});ImGui::NewFrame();DrawTrainingFlyout(v,submit);ImGui::Render();};
  const auto padPress=[&](unsigned held){padFrame(0);padFrame(held);padFrame(0);};
  padPress(MenuInput::Up);
  Check(TrainingNavigation().Focus()=="recording"&&!KeyboardPrompts(),"The pad did not move the training controls or take their prompts");
  padPress(MenuInput::Select);Check(TrainingNavigation().Screen()=="recording","Pad Select did not open a training page");
  padPress(MenuInput::Back);Check(TrainingNavigation().Screen()=="home","Pad Back did not return to the training root");
  TakeForwardedMenuAction();padPress(MenuInput::Back);
  Check(TakeForwardedMenuAction().kind==MenuAction::Close,"Pad Back at the root did not close the training controls");
  SetMenuGlyphs(1,0,0);
 }
 choose("recording");choose("record");press(MenuInput::Select);Check(commands.empty(),"Overwrite default not Cancel");
 choose("record");
 io.AddMousePosEvent(5,5);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();
 Check(TrainingNavigation().Confirming()&&commands.empty()&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Outside click dismissed or submitted training confirmation");
 Check(io.WantCaptureKeyboard&&io.WantCaptureMouse,"Flyout lost input capture outside its bounds");
 press(MenuInput::Right);press(MenuInput::Select);Check(commands.size()==1&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Record returned before command acceptance");
 v.acks.Note(commands.back().requestId,false);frame();Check((TakeForwardedMenuAction().kind!=MenuAction::Close),"Failed command closed training");
 choose("play");Check(commands.back().action==training::Action::Play&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Play command dispatch");
 // Untracked commands applied after it in the same game frame do not hide its result.
 v.acks.Note(commands.back().requestId,true);v.acks.Note(0,false);frame();Check((TakeForwardedMenuAction().kind==MenuAction::Close),"Accepted playback did not return to practice");
 press(MenuInput::Back);Check(TrainingNavigation().Screen()=="home"&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Back skipped the training root");
 // Both sides of the recording grid breakpoint retain list adjustment in
 // the full-width footer. Load must select the file before submitting it.
 for(float width:{800.f,1920.f}){
  io.DisplaySize=ImVec2(width,1080);frame();choose("recording");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});frame();
  Check((rows[0].height==30)==(width==1920),"Recording breakpoint fixture used the wrong layout");
  // Play's Left and Right choose once or looped, without playing.
  reach("play");commands.clear();press(MenuInput::Right);
  Check(commands.size()==1&&commands.back().action==training::Action::Loop&&commands.back().loop,"Play Right did not loop in the recording footer");
  v.acks.Note(commands.back().requestId,true);frame();
  commands.clear();press(MenuInput::Left);
  Check(commands.size()==1&&commands.back().action==training::Action::Loop&&!commands.back().loop,"Play Left did not play once in the recording footer");
  v.acks.Note(commands.back().requestId,true);frame();
  choose("load-recording");commands.clear();press(MenuInput::Right);
  const auto load=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="load-recording";});
  Check(load!=rows.end()&&load->value=="Second"&&commands.empty(),"Load recording Right did not select the second file");
  press(MenuInput::Select);Check(commands.size()==1&&commands.back().action==training::Action::Load&&commands.back().frames.size()==7,"Load recording submitted the wrong selected file");
  commands.clear();press(MenuInput::Left);press(MenuInput::Select);
  Check(commands.size()==1&&commands.back().action==training::Action::Load&&commands.back().frames.size()==3,"Load recording Left did not restore the first file");
  commands.clear();choose("mirror");
  Check(commands.size()==1&&commands.back().action==training::Action::Mirror,"Mirror did not send its command");
  v.acks.Note(commands.back().requestId,true);frame();
  // Select turns a recorded slot off and on, and chooses an empty one for
  // Record. In the grid Up and Down keep to a column, so the slots are focused directly.
  const auto slot=[&](const char* id){TrainingNavigation().Focus(id,rows);frame();Check(TrainingNavigation().Focus()==id,"Slot row unreachable");};
  commands.clear();slot("slot-0");press(MenuInput::Select);
  Check(commands.size()==1&&commands.back().action==training::Action::SlotOn&&commands.back().slot==0&&!commands.back().on,"Select did not turn the recorded slot off");
  v.acks.Note(commands.back().requestId,true);frame();
  commands.clear();slot("slot-1");press(MenuInput::Select);
  Check(commands.size()==1&&commands.back().action==training::Action::Select&&commands.back().slot==1,"Select did not choose the empty slot for Record");
  v.acks.Note(commands.back().requestId,true);frame();
  // Left and Right on Record choose the slot it records into.
  commands.clear();reach("record");press(MenuInput::Right);
  Check(commands.size()==1&&commands.back().action==training::Action::Select&&commands.back().slot==1,"Right on Record did not choose the next slot");
  v.acks.Note(commands.back().requestId,true);frame();
  // The order row: Right picks in order, Left at random.
  commands.clear();reach("order");press(MenuInput::Right);
  Check(commands.size()==1&&commands.back().action==training::Action::PlayOrder&&commands.back().order==training::Order::InOrder,"Order Right did not pick in order");
  v.acks.Note(commands.back().requestId,true);frame();
  commands.clear();press(MenuInput::Left);
  Check(commands.size()==1&&commands.back().action==training::Action::PlayOrder&&commands.back().order==training::Order::Random,"Order Left did not pick at random");
  v.acks.Note(commands.back().requestId,true);frame();
  SetMenuEntriesProbe({});press(MenuInput::Back);
 }
 io.DisplaySize=ImVec2(1280,960);frame();
 press(MenuInput::Back);Check((TakeForwardedMenuAction().kind==MenuAction::Close),"Root Back did not return to practice");
 // F7 whose focus period ended before its opening was accepted: nothing
 // opens and nothing is left asked for, so the next opening under a fresh
 // token shows the controls as they were, with no overwrite question.
 input::TrainingControls controls;
 const auto stale=controls.Sample();controls.LoseFocus();controls.GainFocus();
 Check(!OpenTrainingRecordings(controls,stale)&&!controls.Open(),"F7 under a focus period that ended opened the controls");
 Check(controls.Open(controls.Sample()),"The controls did not open under a fresh token");
 frame();frame();
 Check(TrainingNavigation().Screen()=="home"&&!TrainingNavigation().Confirming(),"A refused F7 left its recordings for the next opening");
 controls.Close();
 // F7 on a recorded slot asks about overwriting it: Record is focused with its
 // question open on Cancel, and Right then Select sends exactly one Record.
 commands.clear();Check(OpenTrainingRecordings(controls,controls.Sample()),"F7 did not open the controls");frame();frame();
 Check(TrainingNavigation().Screen()=="recording"&&TrainingNavigation().Focus()=="record"&&TrainingNavigation().Confirming()&&
  !TrainingNavigation().ConfirmSelected(),"F7 on a recorded slot did not ask before overwriting");
 press(MenuInput::Select);Check(commands.empty()&&!TrainingNavigation().Confirming(),"The overwrite question did not default to Cancel");
 OpenTrainingRecordings(controls,controls.Sample());frame();frame();press(MenuInput::Right);press(MenuInput::Select);
 Check(commands.size()==1&&commands.back().action==training::Action::AdvanceRecording,"Confirming the overwrite did not send one Record");
 TakeForwardedMenuAction();
 // The published action is the logical setting while a reply plays. The
 // row steps from Crouch to Jump, rather than from the temporary Stand.
 press(MenuInput::Back);choose("dummy");
 v.mode=training::Mode::Playback;v.dummy.action=1;
 choose("dummy-action");commands.clear();press(MenuInput::Right);
 Check(commands.size()==1&&commands.back().action==training::Action::DummyState&&commands.back().dummy.action==2,
  "Dummy action row did not edit the logical playback setting");
 press(MenuInput::Back);choose("reply");
 commands.clear();choose("reply-timing");press(MenuInput::Right);
 Check(commands.empty(),"Editing reply settings submitted an invalid saved reply as an empty typed plan");
 const auto saved=[&]{std::ifstream file(folder/"training.json");nlohmann::json practice;file>>practice;return practice;};
 // Even unreadable loaded text must allow Off to reach the runtime.
 choose("reply");press(MenuInput::Right);
 Check(commands.size()==1&&runtimePlan.when==0&&!training::DummyReplies(runtimePlan,1,0)&&
  saved()["reply"]["when"]==0&&saved()["reply"]["moves"]=="not a move","Invalid loaded text prevented Reply Off");
 const auto edit=[&](const char* text){choose("reply-moves");
  Check(TrainingNavigation().Editing(),"Reply editor did not open");TrainingNavigation().Draft(text);
  // InputText keeps a separate buffer while active. Reload the model draft
  // into that buffer before rendering, or it copies the previous text back.
  auto* editor=ImGui::FindWindowByName("###EditText");Check(editor!=nullptr,"Reply editor was not drawn");
  if(auto* input=ImGui::GetInputTextState(editor->GetID("##Draft"))) input->ReloadUserBufAndMoveToEnd();
  frame();Check(TrainingNavigation().Draft()==text,"Reply draft did not survive rendering");press(MenuInput::Select);
  Check(!TrainingNavigation().Editing(),"Reply editor did not accept its draft");};
 // Start with a valid enabled reply, then accept a nonempty line with no
 // moves. Rejection must preserve the editor, file and runtime together.
 edit("623HP");choose("reply");press(MenuInput::Right);
 Check(runtimePlan.when==1&&!runtimePlan.moves.empty()&&training::DummyReplies(runtimePlan,1,0),"Valid reply did not become active");
 const auto active=saved();const auto activeFrames=runtimePlan.moves.size();commands.clear();
 edit("> ,");
 Check(commands.empty()&&saved()==active&&runtimePlan.when==1&&runtimePlan.moves.size()==activeFrames&&
  !TrainingNotice(failed).empty()&&failed,"Rejected reply partially changed saved settings or runtime");
 choose("reply-moves");Check(TrainingNavigation().Draft()=="623HP","Rejected reply replaced the editor's accepted text");press(MenuInput::Back);
 choose("reply");press(MenuInput::Left);
 Check(commands.size()==1&&runtimePlan.when==0&&runtimePlan.moves.empty()&&
  !training::DummyReplies(runtimePlan,1,0)&&saved()["reply"]["when"]==0&&saved()["reply"]["moves"]=="623HP",
  "Reply Off disagreed with the saved settings or runtime after a rejected edit");
 // The editor still validates notation while Off; it cannot persist text
 // that would fail the next time replies are enabled.
 const auto off=saved();commands.clear();edit("> ,");
 Check(commands.empty()&&saved()==off&&runtimePlan.when==0,"Reply Off allowed an invalid edit to be saved");
 // The pick list: the typed reply that is no preset shows as Custom; a preset
 // is sent and saved as the same text typed would be; Custom brings the typed
 // reply back; Select opens the typed field; a preset typed in another
 // spelling shows as that preset.
 {
  const auto focus=[&](const char* id){frame();for(int i=0;i<100&&TrainingNavigation().Focus()!=id;++i)press(MenuInput::Up);
   for(int i=0;i<100&&TrainingNavigation().Focus()!=id;++i)press(MenuInput::Down);Check(TrainingNavigation().Focus()==id,"Training entry unreachable");};
  std::vector<MenuEntry> replyRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){replyRows=rows;});
  const auto pick=[&]{frame();for(const auto& row:replyRows)if(row.id=="reply-move")return row.value;return std::string();};
  focus("reply");press(MenuInput::Right);
  Check(runtimePlan.when==1&&!runtimePlan.moves.empty(),"The typed reply was not turned back on");
  Check(pick()==loc::T("training.reply.custom"),"A typed reply that is no preset did not show as Custom");
  focus("reply-move");commands.clear();press(MenuInput::Right);
  Check(pick()==loc::T("training.reply.recordings")&&saved()["reply"]["moves"]==""&&saved()["reply"]["custom"]=="623HP"&&
   runtimePlan.moves.empty()&&!saved()["reply"].contains("slot"),"Custom did not step on to the recordings");
  press(MenuInput::Right);
  training::DummyPlan typed;std::string error;
  Check(training::BuildReplyPlan("623P",runtimePlan,typed,error),"The first preset is not notation");
  Check(pick()=="623P"&&saved()["reply"]["moves"]=="623P"&&commands.back().action==training::Action::DummyPlan&&
   runtimePlan.moves.size()==typed.moves.size(),"The first preset was not sent and saved as typed text");
  press(MenuInput::Left);press(MenuInput::Left);
  Check(pick()==loc::T("training.reply.custom")&&saved()["reply"]["moves"]=="623HP"&&!runtimePlan.moves.empty(),"Custom did not bring the typed reply back");
  press(MenuInput::Select);frame();
  Check(TrainingNavigation().Editing()&&TrainingNavigation().Focus()=="reply-moves"&&TrainingNavigation().Draft()=="623HP","Select on the pick list did not open the typed field");
  press(MenuInput::Back);Check(!TrainingNavigation().Editing(),"The typed field did not close");
  edit("lp+lk");
  Check(pick()=="LP+LK"&&saved()["reply"]["custom"]=="623HP","A preset typed in another spelling did not show as that preset");
  SetMenuEntriesProbe({});
 }
 // A position is told saved or reset only once the game has done it: a
 // refusal or a failed save after the queue took it never reads as success.
 press(MenuInput::Back);choose("tools");
 const auto told=[&](const char* key,bool wantFailed){bool f=false;const auto text=TrainingNotice(f);return text==loc::T(key)&&f==wantFailed;};
 commands.clear();choose("save-pos");
 Check(commands.size()==1&&commands.back().action==training::Action::Save&&commands.back().requestId!=0,"Save position was not sent as a tracked command");
 const auto save=commands.back().requestId;frame();
 Check(!told("training.position.saved",false),"Position read as saved before the game saved it");
 v.acks.Note(save,false);frame();
 Check(told("training.command_rejected",true),"A failed save did not say so");
 choose("save-pos");v.acks.Note(commands.back().requestId,true);frame();
 Check(told("training.position.saved",false),"A save the game made was not told");
 choose("reset-pos");Check(commands.back().action==training::Action::Restore&&commands.back().requestId!=0,"Reset position was not sent as a tracked command");
 v.acks.Note(commands.back().requestId,false);frame();
 Check(told("training.command_rejected",true),"A refused reset read as done");
 // The keys' result is read with the controls closed, and a tracked command
 // after it in the same game frame does not hide it.
 choose("reset-pos");const auto reset=commands.back().requestId;v.acks.Note(reset,true);v.acks.Note(reset+100,false);
 ImGui::NewFrame();TrainingHotkeys(v,submit);ImGui::Render();
 Check(told("training.position.reset_done",false),"A reset the game made was not told by the hotkeys");
 // The latest attempt owns the notice: a new request clears the last result,
 // and a refused one is not replaced by an older request's success.
 choose("save-pos");const auto older=commands.back().requestId;frame();
 Check(!told("training.position.reset_done",false),"A queued save still showed the previous result");
 queueOpen=false;choose("save-pos");queueOpen=true;
 Check(commands.back().requestId==older&&told("training.command_rejected",true),"A save the queue refused did not say so");
 v.acks.Note(older,true);frame();
 Check(told("training.command_rejected",true),"An older save's success replaced a newer refusal");
 // The pad's save then reset, taken in one batch with no position saved yet:
 // both go to the session in order, which decides the reset once the save
 // before it has run; nothing turns the reset down here first.
 {
  const bool hadCheckpoint=v.checkpoint;v.checkpoint=false;commands.clear();
  input::TrainingPadEvent save,reset;save.kind=input::TrainingPadEvent::Kind::Save;reset.kind=input::TrainingPadEvent::Kind::Reset;
  save.generation=reset.generation=v.generation;save.place[0]=120;save.place[1]=420;
  ImGui::NewFrame();TrainingPadPosition(v,submit,save);TrainingPadPosition(v,submit,reset);ImGui::Render();
  Check(commands.size()==3&&commands[0].action==training::Action::Place&&commands[1].action==training::Action::Save&&
   commands[2].action==training::Action::Restore&&commands[2].requestId!=0,"A reset after a save in the same batch was turned down before the session saw it");
  // The session then refuses the reset only when the save before it failed.
  v.acks.Note(commands[2].requestId,false);frame();
  Check(told("training.command_rejected",true),"A refused reset read as done");
  v.checkpoint=hadCheckpoint;
 }
 // The pad's chord on standby asks for the recording to start, for its own battle.
 {
  commands.clear();input::TrainingPadEvent advance;advance.kind=input::TrainingPadEvent::Kind::Advance;advance.generation=v.generation;
  ImGui::NewFrame();TrainingPadPosition(v,submit,advance);ImGui::Render();
  Check(commands.size()==1&&commands[0].action==training::Action::AdvanceRecording&&commands[0].generation==v.generation,"The pad's chord on standby did not start the recording");
 }
 for(const char* name:{"First","Second"})std::filesystem::remove(folder/"recordings"/(std::string(name)+".json"));
 std::filesystem::remove(folder/"recordings");std::filesystem::remove(folder/"training.json");std::filesystem::remove(folder);
}
