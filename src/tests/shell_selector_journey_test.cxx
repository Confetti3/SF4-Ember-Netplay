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
 at("home");Check(status==loc::T("selection.status_browse"),"The selector's board promised that Select saves");
 for(const char* page:{"stage","random-pool"}){at(page);Check(status==loc::T("selection.status_editable"),"A page where Select saves lost that promise");}
 at("options",false);Check(detail("handicap")==loc::T("selection.locked_detail")&&status==loc::T("selection.status_locked"),"A locked options page kept its editable wording");
 SetMenuEntriesProbe({});SetMenuStatusProbe({});
}
// The selector the shell opens from Profile starts on its board, whatever page it was left on.
void SelectorFromProfile() {
 using namespace sf4e;Harness h;FighterSelector selector;selection::Pick pick;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 h.Screen("card");h.Choose("selection");
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="home","Fighter select did not open on its first page");
 selector.Navigation().Push("options");h.Frame();
 h.Screen("card");h.Choose("selection");
 Check(selector.Navigation().Screen()=="home","Fighter select reopened from Profile on the page it was left on");
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
 h.Screen("card");h.Choose("selection");h.Frame();
 Check(h.shell.Navigation().Screen()=="selection","A Close left by the developer screen popped the next Fighter select");
}
// Random is the grid's last card, where USFIV has its own: the pad and the
// keyboard reach it, Select saves Random and Select on a fighter ends it. A
// winner who keeps their fighter cannot choose it.
void SelectorRandomTile() {
 using namespace sf4e;Harness h;FighterSelector selector;selection::Pick pick;bool random=false,kept=false;
 auto& io=ImGui::GetIO();
 const auto key=[&](ImGuiKey k){h.Frame();io.AddKeyEvent(k,true);h.Frame();io.AddKeyEvent(k,false);h.Frame();};
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true,{},nullptr,nullptr,kept,&random);};h.view.canEditSelection=true;
 h.Screen("card");h.Choose("selection");
 auto& nav=selector.Navigation();
 Check(nav.Screen()=="home"&&nav.Focus()=="fighter-0","Fighter select did not open on the saved fighter");
 // Ryu is the second card of the first line; Random is the fifth of the sixth.
 for(int i=0;i<5;++i)h.Press(MenuInput::Down);
 for(int i=0;i<3;++i)h.Press(MenuInput::Right);
 Check(nav.Focus()=="random-fighter","The pad did not reach the Random card");
 h.Press(MenuInput::Select);
 Check(random&&pick.fighter==0&&nav.Screen()=="home","Select on Random did not save Random");
 // The keyboard: Left to the last fighter, Right back to Random.
 key(ImGuiKey_LeftArrow);
 Check(nav.Focus()=="fighter-"+std::to_string(selection::RosterDisplayOrder[selection::FighterCount-1]),"Left from Random did not reach the last fighter");
 key(ImGuiKey_RightArrow);
 Check(nav.Focus()=="random-fighter","The keyboard did not reach the Random card");
 // Random saved: Select opens the rolled fighter's page, the rest of the pick.
 key(ImGuiKey_Enter);
 Check(random&&nav.Screen()=="look","Select on a saved Random did not open the fighter's page");
 key(ImGuiKey_Backspace);
 // Select on a fighter saves that fighter and ends Random.
 key(ImGuiKey_LeftArrow);key(ImGuiKey_Enter);
 Check(!random&&pick.fighter==selection::RosterDisplayOrder[selection::FighterCount-1]&&nav.Screen()=="look",
  "Select on a fighter did not end Random");
 key(ImGuiKey_Backspace);
 // A winner who keeps their fighter: Random is there, but chooses nothing.
 kept=true;key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(!random&&nav.Focus()=="random-fighter","A winner who keeps their fighter could choose Random");
 // Without a place for the choice (Training's dummy) the grid has no Random card.
 kept=false;bool seen=false;
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){
  seen=seen||std::any_of(rows.begin(),rows.end(),[](const MenuEntry& e){return e.id=="random-fighter";});});
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.Frame(0,2);
 SetMenuEntriesProbe({});
 Check(!seen,"A selector with no Random choice drew the Random card");
}
