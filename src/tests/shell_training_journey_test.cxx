#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"

// Training on Home is the offline command with the game sent on into
// Training mode; Play offline alone sends the game nowhere.
void TrainingFromHome() {
 using namespace sf4e;
 using Kind=netplay::CommandKind;
 Harness h;h.Frame();
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
 const auto before=h.actions.size();
 h.Choose("room-training");
 Check(h.actions.size()==before+1&&h.actions.back().enterTraining&&h.actions.back().command.kind!=Kind::StartOffline,"Training from the room did not ask for Training alone");
 // Seated opposite another fighter and called back: the table page, with the time to ready on its Ready row.
 me.table=0;me.seat=0;me.status=room::MemberStatus::Seated;other.table=0;other.seat=1;other.status=room::MemberStatus::Seated;
 h.view.room.members={me,other};h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;h.view.room.tables[0].phase=room::TablePhase::Waiting;
 h.view.canTrain=false;h.view.trainingCallSequence=1;h.view.trainingReadySeconds=15;h.Frame();
 Check(h.shell.Navigation().Screen()=="room-table","A player called back from Training was not shown their table");
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
}
void TrainingJourneys() {
 using namespace sf4e;
 HeadlessImGui imgui;auto& io=imgui.io;
 training::View v;v.available=v.ready=v.checkpoint=true;v.generation=77;v.lengths[0]=20;
 std::vector<training::Command> commands;
 auto frame=[&](unsigned held=0){io.DeltaTime=1.f/60;SetMenuInput({MenuInput::Select,0});
  const ImGuiKey keys[]={ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_Enter,ImGuiKey_Escape};
  for(unsigned i=0;i<6;++i)io.AddKeyEvent(keys[i],(held&(1u<<i))!=0);
  ImGui::NewFrame();
  DrawTrainingFlyout(v,[&](training::Command c){commands.push_back(c);return true;});ImGui::Render();};
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
}
