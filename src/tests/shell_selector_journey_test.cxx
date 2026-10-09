#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"

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
