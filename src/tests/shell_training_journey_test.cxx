#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include "../session/TrainingCall.hxx"
#include "../training/RecordingFile.hxx"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

// Training on Home is the offline command with the game sent on into
// Training mode; Play offline alone sends the game nowhere.
void TrainingFromHome() {
 using namespace sf4e;
 using Kind=netplay::CommandKind;
 Harness h;h.Frame();
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 h.Choose("settings");
 const std::vector<std::string> settings={"player","defaults","interface","training-replays","discord","about"};
 Check(rows.size()==settings.size(),"Settings row count changed");
 for(std::size_t i=0;i<settings.size();++i)Check(rows[i].id==settings[i],"Settings order changed");
 h.Choose("interface");
 const std::vector<std::string> interfaceRows={"hud","hud-layout","hud-name-offset","hud-size","hud-position","hud-spacing","ready-sound","ready-volume","ready-test","scale","language"};
 Check(rows.size()==interfaceRows.size(),"Interface no longer has Stable's 11 rows");
 for(std::size_t i=0;i<interfaceRows.size();++i)Check(rows[i].id==interfaceRows[i],"Interface order changed");
 h.Press(MenuInput::Back);h.Choose("training-replays");
 Check(rows.size()==3&&rows[0].id=="match-frame-meter"&&rows[1].id=="training-auto-ready"&&rows[2].id=="replay-save-watched","Training and replays settings order wrong");
 Check(rows[1].detail==loc::Tf("settings.training_auto_ready_detail",room::TrainingCall::ReadyWindowMs/1000),"Auto-ready detail lost runtime deadline");
 h.Choose("match-frame-meter");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.matchFrameMeter,"Match frame meter did not save from new screen");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("training-auto-ready");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.trainingAutoReady,"Training auto-ready did not save from new screen");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 SetMenuEntriesProbe({});h.Screen("home");
 h.Choose("training");
 Check(!h.actions.empty()&&h.actions.back().command.kind==Kind::StartOffline&&h.actions.back().enterTraining,"Training did not ride on the offline command");
 h.Screen("home");h.Choose("offline");
 Check(h.actions.back().command.kind==Kind::StartOffline&&!h.actions.back().enterTraining,"Play offline asked for Training");
}
// Training from inside a room: the row sends no room command, and a player
// called back from Training lands on their table, readied for them only once
// the runtime allows a Ready.
void TrainingFromRoom() {
 using namespace sf4e;
 using Kind=netplay::CommandKind;
 Harness h;h.Frame();
 room::Member me,other;me.id=1;me.name="Me";other.id=2;other.name="Other";
 h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;h.view.room.roomEpoch=9;h.view.room.localMember=1;
 for(std::size_t i=0;i<h.view.room.tables.size();++i)h.view.room.tables[i].id=static_cast<std::uint8_t>(i);
 h.view.room.members={me,other};h.view.canTrain=true;h.Frame();
 std::vector<MenuEntry> rows;std::string status;Tone tone=Tone::Neutral;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
 SetMenuStatusProbe([&](const char* text,Tone value){status=text;tone=value;});h.Frame();
 const auto training=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="room-training";});
 Check(training!=rows.end()&&training->label==loc::T("room.training")&&std::next(training)->id=="leave","Wait in Training label or position wrong");
 const auto before=h.actions.size();
 h.Choose("room-training");
 Check(h.actions.size()==before+1&&h.actions.back().enterTraining&&h.actions.back().command.kind!=Kind::StartOffline,"Training from the room did not ask for Training alone");
 // Seated opposite another fighter and called back: the table page, with the time to ready on its Ready row.
 me.table=0;me.seat=0;me.status=room::MemberStatus::Seated;other.table=0;other.seat=1;other.status=room::MemberStatus::Seated;
 h.view.room.members={me,other};h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;h.view.room.tables[0].phase=room::TablePhase::Waiting;
 h.view.canTrain=false;h.view.trainingCallSequence=1;h.view.trainingReadySeconds=15;h.Frame();
 Check(h.shell.Navigation().Screen()=="room-table","A player called back from Training was not shown their table");
 Check(status==loc::Tf("room.training_call.ready_in",15)&&tone==Tone::Pending,"Training deadline missing from table status");
 h.Screen("room");Check(status==loc::Tf("room.training_call.ready_in",15)&&tone==Tone::Pending,"Training deadline missing from room status");h.Screen("room-table");
 h.view.room.tables[0].rules.training=true;h.Frame();
 const auto rules=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="rules";});
 Check(rules!=rows.end()&&rules->value==loc::Tf("room.rules_summary_practice",3,99,loc::T("common.on")),"Non-host cannot see Practice match rule");
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
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
void TrainingJourneys() {
 using namespace sf4e;
 HeadlessImGui imgui;auto& io=imgui.io;
 training::View v;v.available=v.ready=v.checkpoint=true;v.generation=77;v.lengths[0]=20;
 std::vector<training::Command> commands;
 training::DummyPlan runtimePlan;
 const auto submit=[&](training::Command c){
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
 auto frame=[&](unsigned held=0){io.DeltaTime=1.f/60;SetMenuInput({MenuInput::Select,0});
  const ImGuiKey keys[]={ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_Enter,ImGuiKey_Escape};
  for(unsigned i=0;i<6;++i)io.AddKeyEvent(keys[i],(held&(1u<<i))!=0);
  ImGui::NewFrame();
  DrawTrainingFlyout(v,submit);ImGui::Render();};
 auto press=[&](unsigned held){frame();frame(held);frame();};
 auto choose=[&](const char* id){frame();for(int i=0;i<100&&TrainingNavigation().Focus()!=id;++i)press(MenuInput::Up);
  for(int i=0;i<100&&TrainingNavigation().Focus()!=id;++i)press(MenuInput::Down);
  Check(TrainingNavigation().Focus()==id,"Training entry unreachable");press(MenuInput::Select);};
 frame();
 Check(TrainingNavigation().Focus()=="recording","Removed save-position section is still in training navigation");
 press(MenuInput::Down);
 Check(TrainingNavigation().Focus()=="history","Removed frame panel is still in training navigation");
 choose("recording");choose("record");press(MenuInput::Select);Check(commands.empty(),"Overwrite default not Cancel");
 choose("record");
 io.AddMousePosEvent(5,5);frame();io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();
 Check(TrainingNavigation().Confirming()&&commands.empty()&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Outside click dismissed or submitted training confirmation");
 Check(io.WantCaptureKeyboard&&io.WantCaptureMouse,"Flyout lost input capture outside its bounds");
 press(MenuInput::Right);press(MenuInput::Select);Check(commands.size()==1&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Record returned before command acceptance");
 v.commandId=commands.back().requestId;v.commandAccepted=false;v.commandError="Fight not ready";frame();Check((TakeForwardedMenuAction().kind!=MenuAction::Close),"Failed command closed training");
 choose("play");Check(commands.back().action==training::Action::Play&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Play command dispatch");
 v.commandId=commands.back().requestId;v.commandAccepted=true;frame();Check((TakeForwardedMenuAction().kind==MenuAction::Close),"Accepted playback did not return to practice");
 press(MenuInput::Back);Check(TrainingNavigation().Screen()=="home"&&(TakeForwardedMenuAction().kind!=MenuAction::Close),"Back skipped the training root");
 // Both sides of the recording grid breakpoint retain list adjustment in
 // the full-width footer. Load must select the file before submitting it.
 for(float width:{800.f,1920.f}){
  io.DisplaySize=ImVec2(width,1080);frame();choose("recording");
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});frame();
  Check((rows[0].height==30)==(width==1920),"Recording breakpoint fixture used the wrong layout");
  choose("loop");commands.clear();press(MenuInput::Right);
  Check(commands.size()==1&&commands.back().action==training::Action::Loop&&commands.back().value==1,"Loop Right did not adjust in recording footer");
  v.commandId=commands.back().requestId;v.commandAccepted=true;frame();
  commands.clear();press(MenuInput::Left);
  Check(commands.size()==1&&commands.back().action==training::Action::Loop&&commands.back().value==0,"Loop Left did not adjust in recording footer");
  v.commandId=commands.back().requestId;v.commandAccepted=true;frame();
  choose("load-recording");commands.clear();press(MenuInput::Right);
  const auto load=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& row){return row.id=="load-recording";});
  Check(load!=rows.end()&&load->value=="Second"&&commands.empty(),"Load recording Right did not select the second file");
  press(MenuInput::Select);Check(commands.size()==1&&commands.back().action==training::Action::Load&&commands.back().frames.size()==7,"Load recording submitted the wrong selected file");
  commands.clear();press(MenuInput::Left);press(MenuInput::Select);
  Check(commands.size()==1&&commands.back().action==training::Action::Load&&commands.back().frames.size()==3,"Load recording Left did not restore the first file");
  SetMenuEntriesProbe({});press(MenuInput::Back);
 }
 io.DisplaySize=ImVec2(1280,960);frame();
 press(MenuInput::Back);Check((TakeForwardedMenuAction().kind==MenuAction::Close),"Root Back did not return to practice");
 // F7 on a recorded slot asks about overwriting it: Record is focused with its
 // question open on Cancel, and Right then Select sends exactly one Record.
 commands.clear();ShowTrainingRecordings();frame();frame();
 Check(TrainingNavigation().Screen()=="recording"&&TrainingNavigation().Focus()=="record"&&TrainingNavigation().Confirming()&&
  !TrainingNavigation().ConfirmSelected(),"F7 on a recorded slot did not ask before overwriting");
 press(MenuInput::Select);Check(commands.empty()&&!TrainingNavigation().Confirming(),"The overwrite question did not default to Cancel");
 ShowTrainingRecordings();frame();frame();press(MenuInput::Right);press(MenuInput::Select);
 Check(commands.size()==1&&commands.back().action==training::Action::Record,"Confirming the overwrite did not send one Record");
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
 Check(runtimePlan.when==1&&!runtimePlan.moves[0].empty()&&training::DummyReplies(runtimePlan,1,0),"Valid reply did not become active");
 const auto active=saved();const auto activeFrames=runtimePlan.moves[0].size();commands.clear();
 edit("> ,");
 Check(commands.empty()&&saved()==active&&runtimePlan.when==1&&runtimePlan.moves[0].size()==activeFrames&&
  !TrainingNotice(failed).empty()&&failed,"Rejected reply partially changed saved settings or runtime");
 choose("reply-moves");Check(TrainingNavigation().Draft()=="623HP","Rejected reply replaced the editor's accepted text");press(MenuInput::Back);
 choose("reply");press(MenuInput::Left);
 Check(commands.size()==1&&runtimePlan.when==0&&runtimePlan.moves[0].empty()&&runtimePlan.moves[1].empty()&&
  !training::DummyReplies(runtimePlan,1,0)&&saved()["reply"]["when"]==0&&saved()["reply"]["moves"]=="623HP",
  "Reply Off disagreed with the saved settings or runtime after a rejected edit");
 // The editor still validates notation while Off; it cannot persist text
 // that would fail the next time replies are enabled.
 const auto off=saved();commands.clear();edit("> ,");
 Check(commands.empty()&&saved()==off&&runtimePlan.when==0,"Reply Off allowed an invalid edit to be saved");
 for(const char* name:{"First","Second"})std::filesystem::remove(folder/"recordings"/(std::string(name)+".json"));
 std::filesystem::remove(folder/"recordings");std::filesystem::remove(folder/"training.json");std::filesystem::remove(folder);
}
