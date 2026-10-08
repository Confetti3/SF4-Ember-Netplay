#include "shell_journey_support.hxx"
#include "shell_chat_journey.hxx"
#include "shell_replay_journey.hxx"
#include "shell_additional_journeys.hxx"
#include <algorithm>
#include <cstring>
#include <iterator>
namespace {
// A notice raised while an editor or a confirmation is open must be seen, and
// must give the dialog back with its draft and its Cancel default.
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
void NoticeOverDialogs() {
 using namespace sf4e;Harness h;h.Frame();
 const auto shown=[&](const char* name){const auto* w=ImGui::FindWindowByName(name);return w&&w->Active&&!w->Hidden&&w->HiddenFramesCannotSkipItems==0;};
 h.Screen("player");h.Choose("name");
 Check(h.shell.Navigation().Editing(),"The name editor did not open");
 h.Frame(0,2);Check(shown("###EditText"),"The editor was not drawn");
 h.shell.Navigation().Draft("Half typed");
 h.view.readyFailure="Your Ready did not go through.";h.view.readyFailureSequence=1;h.Frame(0,3);
 Check(shown("###Notice"),"A notice raised over the editor was never drawn");
 Check(h.shell.Navigation().Editing()&&h.shell.Navigation().Draft()=="Half typed","A notice cost the editor its draft");
 h.Press(MenuInput::Select);h.Frame(0,3);
 Check(!shown("###Notice")&&shown("###EditText")&&h.shell.Navigation().Editing()&&h.shell.Navigation().Draft()=="Half typed",
  "Answering the notice did not give the editor back");
 h.Press(MenuInput::Back);Check(!h.shell.Navigation().Editing(),"Back did not cancel the returned editor");
 // The same over a confirmation, and Use keyboard is one: its answer starts on Cancel.
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
 h.Screen("player");h.Frame();SetMenuEntriesProbe({});
 const auto keyboard=std::find_if(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="keyboard";});
 Check(keyboard!=rows.end()&&keyboard->confirm&&keyboard->detail==loc::T("player.use_keyboard_detail"),"Use keyboard is not a confirmation that says what it costs");
 h.Choose("keyboard");
 Check(h.shell.Navigation().Confirming()&&!h.shell.Navigation().ConfirmSelected(),"Use keyboard did not open a question defaulting to Cancel");
 h.Frame(0,2);Check(shown("###ConfirmAction"),"The confirmation was not drawn");
 h.view.readyFailure="Your Ready did not go through again.";h.view.readyFailureSequence=2;h.Frame(0,3);
 Check(shown("###Notice")&&h.shell.Navigation().Confirming(),"A notice raised over the confirmation was never drawn");
 h.Press(MenuInput::Select);h.Frame(0,3);
 Check(!shown("###Notice")&&shown("###ConfirmAction")&&h.shell.Navigation().Confirming()&&!h.shell.Navigation().ConfirmSelected(),
  "Answering the notice did not give the confirmation back on Cancel");
 auto sent=h.actions.size();h.Press(MenuInput::Select);
 Check(h.actions.size()==sent&&!h.shell.Navigation().Confirming(),"Cancel on Use keyboard still switched the device");
 h.Choose("keyboard");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==sent+1&&h.actions.back().inputAction==input::Action::UseKeyboard,"Confirming Use keyboard did not switch the device");
}
// The failed language save speaks on the Interface screen only, and everything
// more urgent outranks it.
void LanguageSaveFailure() {
 using namespace sf4e;Harness h;
 h.shell.SetLanguageSaver([](const std::string&,std::string& diagnostic){diagnostic="Read-only folder";return false;});
 std::string status;Tone tone=Tone::Neutral;
 SetMenuStatusProbe([&](const char* text,Tone t){status=text;tone=t;});
 h.Screen("interface");h.FocusOn("language");h.Press(MenuInput::Right);h.Frame(0,45);
 const std::string failure=loc::T("settings.language_save_failed");
 Check(status==failure&&tone==Tone::Error,"A failed language save was not reported where the language is set");
 h.Screen("join");h.Frame(0,2);Check(status!=failure,"The language save failure followed the player to another screen");
 h.view.session.error="Could not join the room.";h.Frame();
 Check(status==h.view.session.error,"A session error lost to a stale language save failure");
 h.view.session.error.clear();h.view.controllerUnavailable=true;h.Frame();
 Check(status==loc::T("controller.disconnected"),"A controller disconnect lost to a stale language save failure");
 h.view.controllerUnavailable=false;h.Screen("interface");h.Frame(0,2);
 Check(status==failure,"The failure did not return with the Interface screen");
 SetMenuStatusProbe({});loc::SetActive(loc::Locale::En);
}
// What the session reports is worded in the active language, and a replacement
// offer outranks the condition that led to it.
void SessionReports() {
 using namespace sf4e;Harness h;
 std::string status;Tone tone=Tone::Neutral;
 SetMenuStatusProbe([&](const char* text,Tone t){status=text;tone=t;});
 h.Screen("home");
 h.view.session.fault=netplay::Fault::ControlRecovering;h.Frame();
 Check(status==loc::T("room.control_recovering")&&tone==Tone::Error,"A lost room control was not worded from the catalog");
 h.view.session.fault=netplay::Fault::CatchingUp;h.Frame();
 Check(status==loc::T("room.catching_up"),"A room that is catching up was not worded from the catalog");
 loc::SetActive(loc::Locale::Fr);h.Frame();
 Check(status==loc::T("room.catching_up")&&status.find("catching up")==std::string::npos,"The catching-up report stayed English in French");
 loc::SetActive(loc::Locale::En);
 h.view.session.room=netplay::RoomState::Joined;h.view.room.roomEpoch=3;h.view.room.localMember=1;
 h.view.session.control=netplay::Health::Lost;h.view.session.recovery=netplay::Recovery::Recovering;
 h.view.session.fault=netplay::Fault::ControlRecovering;h.Screen("room");h.Frame();
 Check(status==loc::T("room.control_recovering"),"The room's recovery status is not the catalog's");
 h.view.session.recovery=netplay::Recovery::ReplacementOffered;h.Frame();
 Check(status==loc::T("room.control_unavailable"),"The offer to replace the room is hidden behind the recovery sentence");
 SetMenuStatusProbe({});
}
// The recovery window says all of a long launcher message, and shows the
// newer of the launcher's message and a service's.
void RecoveryWindow() {
 using namespace sf4e;HeadlessImGui imgui;auto& io=imgui.io;
 const std::string paths="C:\\Program Files (x86)\\Steam\\steamapps\\common\\Super Street Fighter IV - Arcade Edition\\GGPO.dll\n"
  "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Super Street Fighter IV - Arcade Edition\\spdlog.dll";
 const auto message=loc::Tf("launcher.runtime_shadowed",paths);
 float text=0,room=0;bool reported=false;
 SetMenuTextProbe([&](const char* id,float t,float interior,float,float){if(!std::strcmp(id,"command-feedback")){reported=true;text=t;room=interior;}});
 platform::ServiceSnapshot state;
 for(const float dpi:{1.f,1.5f,2.f}){
  // The window is 940x720 at 96 dpi and grows with the display, so the same room at every scale.
  ApplyTheme(dpi);io.Fonts->Build();io.DisplaySize=ImVec2(924*dpi,681*dpi);
  GameMenu menu;menu.navigation=RecoveryNavigation(false);reported=false;
  for(int i=0;i<3;++i){ImGui::NewFrame();DrawRecoveryMenu(menu,state,message,false);ImGui::Render();}
  const auto* window=ImGui::FindWindowByName("###EmberRecovery");
  Check(reported&&window&&window->ScrollMax.y<1,"The recovery window overflowed with a long launcher message");
  Check(text>4*ImGui::GetTextLineHeight(),"The long launcher message did not wrap to several lines");
  Check(text<=room+.5f,"The recovery window cut off the end of the launcher message at 100%, 150% or 200%");
 }
 SetMenuTextProbe({});ApplyTheme(1.f);io.Fonts->Build();io.DisplaySize=ImVec2(1280,960);
 // Newest wins: a rejected folder is not buried by an earlier update check.
 std::string status;Tone tone=Tone::Neutral;SetMenuStatusProbe([&](const char* s,Tone t){status=s;tone=t;});
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
 GameMenu menu;menu.navigation=RecoveryNavigation(false);
 const auto draw=[&](const std::string& launcher,Tone launcherTone,bool updates,bool canStart,bool serviceNewer){
  for(int i=0;i<2;++i){ImGui::NewFrame();DrawRecoveryMenu(menu,state,launcher,updates,launcherTone,canStart,serviceNewer);ImGui::Render();}};
 state.message="You are up to date.";state.succeeded=true;
 draw("That folder does not contain SSFIV.exe.",Tone::Error,false,false,false);
 Check(status=="That folder does not contain SSFIV.exe."&&tone==Tone::Error,"An old update check hid the rejected folder");
 draw("That folder does not contain SSFIV.exe.",Tone::Error,false,false,true);
 Check(status=="You are up to date."&&tone==Tone::Success,"A newer update check was not shown");
 draw("",Tone::Error,false,false,false);
 Check(status=="You are up to date.","With no launcher message the service result was hidden");
 // The launch message that opened recovery keeps its own tone.
 state.message.clear();
 draw(loc::T("launcher.recovery_opened"),Tone::Neutral,false,false,false);
 Check(tone==Tone::Neutral&&status==loc::T("launcher.recovery_opened"),"The recovery notice was shown as an error");
 // The updater can start the game only when told it may, and says what its Close does.
 const auto row=[&](const char* id)->const MenuEntry*{const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});return it==rows.end()?nullptr:&*it;};
 draw("",Tone::Error,true,false,true);
 Check(!row("retry")&&row("close")&&row("close")->detail==loc::T("updates.close_detail"),"The updater offered Start SF4 when it may not, or kept the recovery Close text");
 draw("",Tone::Error,true,true,true);
 Check(row("retry")&&row("retry")->label==loc::T("updates.start_game")&&row("retry")->enabled,"The updater did not offer Start SF4 when it may");
 draw("",Tone::Error,false,true,true);
 Check(row("retry")&&row("retry")->label==loc::T("recovery.retry"),"The launch window's Retry changed");
 // Selecting Start SF4 answers Retry.
 menu.navigation=RecoveryNavigation(true);
 RecoveryChoice choice=RecoveryChoice::None;
 const auto frame=[&](unsigned held=0){SetMenuInput({held,0});ImGui::NewFrame();choice=DrawRecoveryMenu(menu,state,"",true,Tone::Error,true,true);ImGui::Render();};
 frame();frame();
 for(int i=0;i<20&&menu.navigation.Focus()!="retry";++i){frame(MenuInput::Down);frame();}
 Check(menu.navigation.Focus()=="retry","Start SF4 is unreachable in the updater");
 frame(MenuInput::Select);
 Check(choice==RecoveryChoice::Retry,"Start SF4 did not answer Retry");
 SetMenuStatusProbe({});SetMenuEntriesProbe({});
}
// Options rows save as they step, so they do not promise a Select; the status
// line promises what each page does.
void SelectorPages() {
 using namespace sf4e;Harness h;FighterSelector selector;selection::Pick pick;
 selection::Availability available;available.ready=true;available.costumes=3;available.colors[0]=available.colors[1]=5;available.personalActions=1;
 std::vector<MenuEntry> rows;std::string status;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});SetMenuStatusProbe([&](const char* s,Tone){status=s;});
 const auto frame=[&](bool editable=true){SetMenuInput({0,0});ImGui::NewFrame();
  ImGui::Begin("Selector pages");selector.Draw(pick,true,nullptr,[&](int){return available;},nullptr,editable);ImGui::End();ImGui::Render();};
 const auto at=[&](const char* screen,bool editable=true){selector.Navigation().Home();if(std::string(screen)!="home")selector.Navigation().Push(screen);frame(editable);frame(editable);};
 const auto detail=[&](const char* id){return std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;})->detail;};
 at("options");
 Check(detail("handicap")==loc::T("selection.adjust_saves")&&detail("quote")==loc::T("selection.adjust_saves")&&
  detail("handicap").find("Select saves")==std::string::npos,"An option row promised that Select saves");
 Check(status==loc::T("selection.status_adjust"),"The options page promised Select in its status");
 at("home");Check(status==loc::T("selection.status_browse"),"The selector's home promised that Select saves");
 at("appearance");Check(status==loc::T("selection.status_browse"),"The appearance page promised that Select saves");
 for(const char* page:{"roster","costumes","colors","ultra","stage"}){at(page);Check(status==loc::T("selection.status_editable"),"A page where Select saves lost that promise");}
 at("options",false);Check(detail("handicap")==loc::T("selection.locked_detail")&&status==loc::T("selection.status_locked"),"A locked options page kept its editable wording");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// The selector the shell opens from Home starts on its first page too.
void SelectorFromHome() {
 using namespace sf4e;Harness h;FighterSelector selector;selection::Pick pick;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 h.Screen("home");h.Choose("selection");
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="home","Fighter select did not open on its first page");
 selector.Navigation().Push("appearance");selector.Navigation().Push("colors");h.Frame();
 h.Screen("home");h.Choose("selection");
 Check(selector.Navigation().Screen()=="home","Fighter select reopened from Home on the page it was left on");
}
// The developer screen draws its own fighter selectors. They neither inherit
// the room's hints and Back label nor leave a Close behind that would pop the
// player's next Fighter select on its first frame.
void DeveloperSelectors() {
 using namespace sf4e;Harness h;FighterSelector inspector,selector;selection::Pick pick,inspected;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 h.developer=[&]{inspector.Draw(inspected,false,nullptr,{},nullptr,true);};
 h.view.session.room=netplay::RoomState::Joined;h.view.room.roomEpoch=1;h.Frame();
 h.Screen("selection");
 Check(EmbeddedReturnContext().shortcutHints.size()==3,"A selector in a room does not show the room's hints");
 h.Screen("developer");
 Check(EmbeddedReturnContext().shortcutHints.empty(),"The developer screen's selector inherited the room's shortcut hints");
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="home","Back did not leave the developer screen");
 Check(TakeForwardedMenuAction().kind==MenuAction::None,"The developer screen's selector left a Close behind");
 h.Choose("selection");h.Frame();
 Check(h.shell.Navigation().Screen()=="selection","A Close left by the developer screen popped the next Fighter select");
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
}
int main(){try{RoomJourneys();ReplayJourneys();TrainingFromHome();TrainingFromRoom();KeyboardJourneys();ChatJourneys();NoticeOverDialogs();LanguageSaveFailure();SessionReports();RecoveryWindow();SelectorPages();SelectorFromHome();DeveloperSelectors();TrainingJourneys();PresentationJourneys();AppearanceGalleries();std::cout<<"Shell journeys through the renderer passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
