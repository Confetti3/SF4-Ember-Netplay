#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"

// A notice raised while an editor or a confirmation is open must be seen, and
// must give the dialog back with its draft and its Cancel default.
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
 JoinedRoom(h,3);h.view.session.control=netplay::Health::Lost;h.view.session.recovery=netplay::Recovery::Recovering;
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
// Problem reports start off, and Left and Right save the setting; Sent reports
// says when there are none. In the launch window a crash's report is offered
// beside Retry without taking the focus: Send this report opens its preview,
// and Always send goes only through a confirmation that starts on Cancel.
void ProblemReportJourneys() {
 using namespace sf4e;
 // The launch window's rows come from the report worker: nothing while an
 // automatic send is still being allowed, then on its way, then how it went.
 {
  platform::ServiceSnapshot worker;
  Check(!CrashReportView(worker).asking&&!CrashReportView(worker).sending,"A window with no report shows one");
  worker.reporting.automatic=true;worker.reporting.phase=reports::Phase::Preparing;worker.pending=true;
  auto view=CrashReportView(worker);
  Check(!view.asking&&!view.sending&&!view.outcome&&view.failure.empty(),"An automatic send showed before it was allowed");
  worker.reporting.authorized=true;worker.reporting.phase=reports::Phase::Submitting;
  Check(CrashReportView(worker).sending,"An allowed automatic send does not show it is on its way");
  worker.pending=false;worker.reporting.phase=reports::Phase::Sent;worker.reporting.record=reports::Record{};worker.reporting.record->status=reports::Status::Received;
  Check(CrashReportView(worker).outcome&&!CrashReportView(worker).sending,"A finished send does not say how it went");
  worker.reporting={};worker.reporting.automatic=true;worker.reporting.offer=true;worker.reporting.phase=reports::Phase::Idle;worker.reporting.preparation=1;
  Check(CrashReportView(worker).asking,"A report that was not allowed is not offered");
  worker.reporting.offer=false;worker.reporting.automatic=false;worker.reporting.phase=reports::Phase::Failed;worker.reporting.message="failed";
  Check(CrashReportView(worker).failure=="failed","A send from the preview that stopped early says nothing");
 }
 {
  Harness h;h.Frame();
  std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& shown){rows=shown;});
  h.Choose("settings");h.Choose("problem-reports");
  Check(h.shell.Navigation().Screen()=="problem-reports"&&rows.size()==2&&rows[0].id=="send-reports","Problem reports did not open from Settings");
  Check(rows[0].value==loc::T("common.off")&&!h.view.preferences.sendProblemReports,"Problem reports are not off at first");
  h.FocusOn("send-reports");h.Press(MenuInput::Right);h.Frame(0,45);
  Check(!h.actions.empty()&&h.actions.back().command.kind==netplay::CommandKind::SavePreferences&&h.actions.back().preferences.sendProblemReports,
   "Turning problem reports on did not save");
  h.view.preferences=h.actions.back().preferences;h.Frame();
  Check(rows[0].value==loc::T("common.on"),"The setting does not show it is on");
  h.FocusOn("send-reports");h.Press(MenuInput::Left);h.Frame(0,45);
  Check(h.actions.back().command.kind==netplay::CommandKind::SavePreferences&&!h.actions.back().preferences.sendProblemReports,"Turning problem reports off did not save");
  h.view.preferences=h.actions.back().preferences;h.Frame();
  h.Choose("sent-reports");
  Check(h.shell.Navigation().Screen()=="sent-reports"&&rows.size()==1&&rows[0].id=="sent-none","Sent reports does not say there are none");
  // Discord keeps only its own rows.
  h.Screen("discord");h.Frame();
  Check(std::none_of(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="crash-reports"||e.id=="send-reports";}),"Discord still holds a report setting");
  SetMenuEntriesProbe({});
 }
 HeadlessImGui imgui;auto& io=imgui.io;
 const std::string message=loc::T("launcher.game_crashed");
 platform::ServiceSnapshot state;RecoveryReport report;report.asking=true;
 float text=0,room=0;bool reported=false;
 SetMenuTextProbe([&](const char* id,float t,float interior,float,float){if(!std::strcmp(id,"command-feedback")){reported=true;text=t;room=interior;}});
 for(const float dpi:{1.f,1.5f,2.f}){
  ApplyTheme(dpi);io.Fonts->Build();io.DisplaySize=ImVec2(924*dpi,681*dpi);
  GameMenu menu;menu.navigation=RecoveryNavigation(false);reported=false;
  for(int i=0;i<3;++i){ImGui::NewFrame();DrawRecoveryMenu(menu,state,message,false,Tone::Error,false,false,nullptr,report);ImGui::Render();}
  const auto* window=ImGui::FindWindowByName("###EmberRecovery");
  Check(reported&&window&&window->ScrollMax.y<1&&text<=room+.5f,"The crash message and its report offer overflowed the launch window");
 }
 SetMenuTextProbe({});ApplyTheme(1.f);io.Fonts->Build();io.DisplaySize=ImVec2(1280,960);
 GameMenu menu;menu.navigation=RecoveryNavigation(false);
 std::vector<RecoveryChoice> chosen;
 const auto frame=[&](unsigned held=0){SetMenuInput({held,0});ImGui::NewFrame();
  const auto choice=DrawRecoveryMenu(menu,state,message,false,Tone::Error,false,false,nullptr,report);if(choice!=RecoveryChoice::None)chosen.push_back(choice);ImGui::Render();};
 frame();frame();
 Check(menu.navigation.Focus()!="report"&&menu.navigation.Focus()!="report-always"&&chosen.empty(),"The report offer took the focus or answered by itself");
 const auto reach=[&](const char* id){for(int i=0;i<20&&menu.navigation.Focus()!=id;++i){frame(MenuInput::Down);frame();}return menu.navigation.Focus()==id;};
 // Send this report opens the preview at once: nothing is sent from here.
 Check(reach("report"),"Send this report is unreachable");
 frame(MenuInput::Select);frame();
 Check(!menu.navigation.Confirming()&&chosen.size()==1&&chosen[0]==RecoveryChoice::Report,"Send this report did not open the preview");
 chosen.clear();
 Check(reach("report-always"),"Always send is unreachable");
 frame(MenuInput::Select);frame();
 Check(menu.navigation.Confirming()&&!menu.navigation.ConfirmSelected()&&chosen.empty(),"Always send's confirmation does not start on Cancel");
 // Select on Cancel sends nothing.
 frame(MenuInput::Select);frame();
 Check(!menu.navigation.Confirming()&&chosen.empty(),"Cancel chose Always send");
 frame(MenuInput::Select);frame();frame(MenuInput::Right);frame();frame(MenuInput::Select);frame();
 Check(chosen.size()==1&&chosen[0]==RecoveryChoice::AlwaysSend,"Confirming did not answer Always send");
 // Once sent, the offer is gone and one row says how it went.
 report={};report.sending=true;state.pending=true;state.lastAction=platform::ServiceAction::SendReport;
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
 frame();frame();
 const auto has=[&](const char* id){return std::any_of(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});};
 Check(!has("report")&&!has("report-always")&&has("report-status"),"The offer stayed while the report was sending");
 SetMenuEntriesProbe({});
}
