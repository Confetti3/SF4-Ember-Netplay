#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include <algorithm>
#include <iterator>
// A keyboard player reaches everything a pad does: F, T and C are X, Y and
// View, Escape always goes back, and Delete leaves the seat that B leaves.
void KeyboardJourneys(){
 using namespace sf4e;
 Harness h;h.Frame();
 auto& io=ImGui::GetIO();
 const auto key=[&](ImGuiKey k){h.Frame();io.AddKeyEvent(k,true);h.Frame();io.AddKeyEvent(k,false);h.Frame();};
 SetMenuGlyphs(input::PadKeyboard,0,0);
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;
 h.view.room.roomEpoch=10;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Keys";h.view.room.revision=3;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 h.view.room.tables[0].p1=1;
 h.Screen("room");h.FocusOn("table-0");
 std::set<std::string> legend;
 SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))legend.insert(id+7);});
 h.Frame();SetMenuTextProbe({});
 Check(KeyboardPrompts(),"A keyboard player does not see keyboard prompts");
 Check(legend.count(loc::T("room.leave_seat"))&&legend.count(loc::T("room.legend_fighter"))&&legend.count(loc::T("common.back")),
  "The keyboard legend lacks Delete's Leave seat, F's Fighter or Escape's Back");
 // Escape on your own card goes back to Home and keeps the seat.
 auto before=h.actions.size();key(ImGuiKey_Escape);
 Check(h.shell.Navigation().Screen()=="home"&&h.actions.size()==before,"Escape on your own card left the seat");
 h.Screen("room");h.FocusOn("table-0");
 // Delete there leaves it, like a pad's B.
 key(ImGuiKey_Delete);
 Check(h.actions.size()==before+1&&h.actions.back().roomAction.kind==room::ActionKind::Unqueue,"Delete on your own card did not leave the seat");
 // Delete anywhere else does nothing.
 before=h.actions.size();h.FocusOn("copy");key(ImGuiKey_Delete);
 Check(h.actions.size()==before&&h.shell.Navigation().Screen()=="room","Delete away from your card did something");
 // T and C open table options and chat, and again return to the board.
 key(ImGuiKey_T);Check(h.shell.Navigation().Screen()=="room-table","T did not open the table options");
 key(ImGuiKey_T);Check(h.shell.Navigation().Screen()=="room","T again did not return to the board");
 key(ImGuiKey_C);Check(h.shell.Navigation().Screen()=="room-chat","C did not open chat");
 // The message box has the keyboard from the start, so F, T, C and Backspace
 // are text and neither open anything nor cancel the draft; Escape goes back.
 h.Frame(0,4);io.AddInputCharactersUTF8("fct");h.Frame();key(ImGuiKey_F);key(ImGuiKey_T);key(ImGuiKey_Backspace);key(ImGuiKey_Space);
 Check(h.shell.Navigation().Screen()=="room-chat","Typing in chat pressed a menu key");
 key(ImGuiKey_Escape);Check(h.shell.Navigation().Screen()=="room","Escape did not return to the board");
 // F opens fighter select on the roster, at the current fighter; picking
 // one goes on to its Ultra, and the Ultra returns to the room.
 FighterSelector selector;selection::Pick pick;pick.fighter=4;pick.edition=14;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 key(ImGuiKey_F);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="roster"&&selector.Navigation().Focus()=="fighter-4",
  "F did not open the roster at the current fighter");
 key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(pick.fighter==DisplayedAfter(4)&&selector.Navigation().Screen()=="ultra","Picking a fighter did not go on to its Ultra");
 key(ImGuiKey_RightArrow);key(ImGuiKey_KeypadEnter);
 Check(pick.ultra==1&&h.shell.Navigation().Screen()=="room","Picking the Ultra did not return to the room");
 // Back from the roster returns to the room as well.
 key(ImGuiKey_F);key(ImGuiKey_Backspace);
 Check(h.shell.Navigation().Screen()=="room","Back from the roster did not return to the room");
 // Select on the table page's Ultra opens the Ultra cards, and picking one
 // returns to the table page.
 h.view.ultraSteps=true;h.view.ultraName="Ultra II";key(ImGuiKey_T);h.FocusOn("ultra");key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="ultra"&&selector.Navigation().Focus()=="ultra-1",
  "Select on the table's Ultra did not open the Ultra cards at the saved one");
 key(ImGuiKey_LeftArrow);key(ImGuiKey_Enter);
 Check(pick.ultra==0&&h.shell.Navigation().Screen()=="room-table","Picking an Ultra did not return to the table page");
 // Select on the table page's Appearance opens the costume cards at the saved
 // costume; a costume goes on to its colors, and a color returns to the table page.
 selection::Availability available;available.ready=true;available.costumes=3;available.colors[0]=available.colors[1]=5;available.personalActions=1;
 h.selection=[&]{selector.Draw(pick,false,nullptr,[&](int){return available;},nullptr,true);};
 h.view.appearanceName="Original / Color 1";h.FocusOn("appearance");key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="costumes"&&selector.Navigation().Focus()=="costume-0",
  "Select on the table's Appearance did not open the costume cards at the saved one");
 key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(pick.costume==1&&selector.Navigation().Screen()=="colors"&&selector.Navigation().Focus()=="color-"+std::to_string(pick.color),
  "Picking a costume did not go on to its colors at the kept color");
 const auto colors=selection::AllowedColors(pick.fighter,1,available);
 Check(colors.size()>1&&pick.color==colors[0],"The test costume has too few colors");
 key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(pick.color==colors[1]&&h.shell.Navigation().Screen()=="room-table","Picking a color did not return to the table page");
 // Back from the colors goes to the costumes, and Back from there to the table page.
 h.FocusOn("appearance");key(ImGuiKey_Enter);key(ImGuiKey_Enter);
 Check(selector.Navigation().Screen()=="colors","Picking the saved costume again did not open its colors");
 key(ImGuiKey_Backspace);Check(selector.Navigation().Screen()=="costumes","Back from the colors did not return to the costumes");
 key(ImGuiKey_Backspace);Check(h.shell.Navigation().Screen()=="room-table","Back from the costumes did not return to the table page");
 Check(pick.costume==1&&pick.color==colors[1],"Backing out of the galleries changed the pick");
 // The rest of the pick is on the table page too: Additional options opens its
 // page (personal action, win quote and the others), and Back returns.
 h.FocusOn("fighter-options");key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="options",
  "Select on the table's Additional options did not open the options page");
 key(ImGuiKey_Backspace);Check(h.shell.Navigation().Screen()=="room-table","Back from the options page did not return to the table page");
 // P1 opens the stage cards at the saved stage, and a pick returns to the table page.
 int stage=0;h.view.localSlot=0;h.view.stageName="Random";
 h.selection=[&]{selector.Draw(pick,false,nullptr,[&](int){return available;},&stage,true);};
 // Stage shows its value but steps nothing in place: Left and Right stay here.
 h.FocusOn("stage");key(ImGuiKey_RightArrow);key(ImGuiKey_LeftArrow);
 Check(h.shell.Navigation().Screen()=="room-table"&&stage==0,"Left or Right on the table's Stage left the table page");
 key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="stage",
  "Select on the table's Stage did not open the stage cards");
 key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(stage!=0&&h.shell.Navigation().Screen()=="room-table","Picking a stage did not return to the table page");
 key(ImGuiKey_Escape);
 // A pad press puts the pad's prompts back; a key brings the keys again.
 SetMenuGlyphs(input::PadXInput,input::xinput::A,input::xinput::B);
 h.Press(MenuInput::Down);Check(!KeyboardPrompts(),"A pad press left keyboard prompts up");
 key(ImGuiKey_UpArrow);Check(KeyboardPrompts(),"A key press kept the pad's prompts");
 // Typing in a text field counts too, though its keys press no menu bit.
 h.Press(MenuInput::Down);Check(!KeyboardPrompts(),"A pad press left keyboard prompts up");
 h.Screen("room-chat");h.Frame(0,4);
 io.AddInputCharactersUTF8("x");h.Frame();h.Frame();
 Check(KeyboardPrompts(),"Typing with a pad assigned kept the pad's prompts");
 key(ImGuiKey_Escape);
 h.selection=[]{};
}
void PresentationJourneys(){
 Check(MenuScreenLabel("room-members")=="Members"&&MenuScreenLabel("player")=="Player & controller","Internal screen keys leaked into Back labels");
 auto text=TextRow("name","Name","Player",31);auto value=Value("delay","Delay","2","Frames");
 auto confirm=ConfirmRow("leave","Leave room","Disconnect");
 Check(std::strcmp(MenuPrimaryHint(&text),"Edit")==0&&MenuPrimaryHint(&value)==nullptr&&std::strcmp(MenuPrimaryHint(&confirm),"Review")==0,"Contextual legend does not match action");
 text.enabled=false;Check(MenuPrimaryHint(&text)==nullptr,"Disabled field advertises submission");
 Harness h;std::string status;SetMenuStatusProbe([&](const char* text,Tone){status=text;});
 h.view.preferences.showMatchHud=true;h.Frame(); // The HUD defaults to off; the click below must change a saved value.
 h.Screen("interface");ImVec2 leftArrow,otherRow;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){
  if(std::strcmp(id,"hud")==0)leftArrow=ImVec2(min.x+(max.x-min.x)*.75f,(min.y+max.y)*.5f);
  if(std::strcmp(id,"scale")==0)otherRow=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);
 });h.Frame();SetMenuCardProbe({});
 auto& io=ImGui::GetIO();io.AddMousePosEvent(otherRow.x,otherRow.y);h.Frame();
 Check(h.shell.Navigation().Focus()=="hud","Mouse hover stole controller focus");
 io.AddMousePosEvent(leftArrow.x,leftArrow.y);h.Frame();io.AddMouseButtonEvent(0,true);h.Frame();
 io.AddMouseButtonEvent(0,false);h.Frame(0,40);
 Check(!h.actions.empty()&&h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.showMatchHud,"Visible value arrows did not adjust on click");
 Check(status=="Saving...","Queued settings falsely reported Saved before acknowledgement");
 h.view.preferences=h.actions.back().preferences;h.Frame();Check(status=="Saved","Acknowledged settings did not report Saved");h.Press(MenuInput::Down);
 Check(h.shell.Navigation().Focus()=="hud-layout","Controller did not move to the row following the mouse-selected row");
 // 0 frames of input delay is withdrawn: Left from a chosen 1 goes to Auto, keeping 1, never to 0.
 h.view.preferences.inputDelay=1;h.view.preferences.autoInputDelay=false;h.Screen("defaults");h.FocusOn("delay");const auto delaySaves=h.actions.size();
 h.Press(MenuInput::Left);h.Frame(0,40);
 Check(h.actions.size()>delaySaves&&h.actions.back().preferences.autoInputDelay&&h.actions.back().preferences.inputDelay==1,
  "Gameplay defaults offered 0 frames of input delay");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 h.Screen("profile");h.Choose("main-character");h.Press(MenuInput::Right);h.Press(MenuInput::Select);h.Frame(0,40);
 h.view.preferences=h.actions.back().preferences;h.Frame(0,3);Check(status.find("Profile portrait saved:")==0,"Profile success notice missing");
 h.Frame(0,200);Check(status=="Saved","Success notice did not expire");SetMenuStatusProbe({});
 GameMenu recovery;recovery.navigation=RecoveryNavigation(true);sf4e::platform::ServiceSnapshot state;
 state.update.ok=state.update.updateAvailable=true;state.update.expectedSha256=std::string(64,'a');state.update.latestVersion="v9.9.9";
 std::vector<MenuEntry> updateRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){updateRows=r;});
 const auto frame=[&](unsigned held=0){SetMenuInput({held,0});ImGui::NewFrame();const auto choice=DrawRecoveryMenu(recovery,state,"",true);ImGui::Render();return choice;};
 frame();frame();
 // A found update is the first row, named by its version, so Select installs it.
 Check(!updateRows.empty()&&updateRows[0].id=="install"&&updateRows[0].value=="v9.9.9"&&recovery.navigation.Focus()=="install",
  "The updater did not offer the found update first");
 frame(MenuInput::Select);frame();
 Check(recovery.navigation.Confirming()&&!recovery.navigation.ConfirmSelected(),"Recovery install not defaulting to Cancel");
 Check(frame(MenuInput::Select)==RecoveryChoice::None,"Recovery default confirmation installed an update");frame();
 state.pending=true;state.stageDone=100;state.stageTotal=200;frame();
 for(int i=0;i<10&&recovery.navigation.Focus()!="cancel";++i){frame(MenuInput::Down);frame();}
 Check(frame(MenuInput::Select)==RecoveryChoice::Cancel,"Recovery cancellation not reachable");
 // An update found while the window is open takes the highlight once; after
 // that the player's own movement stands, until a newer version is found.
 GameMenu found;found.navigation=RecoveryNavigation(true);sf4e::platform::ServiceSnapshot checking;checking.pending=true;std::string offered;
 const auto show=[&](unsigned held=0){OfferFoundUpdate(found,checking,offered);SetMenuInput({held,0});ImGui::NewFrame();DrawRecoveryMenu(found,checking,"",true);ImGui::Render();};
 show();show();Check(found.navigation.Focus()=="check","The updater did not start on its check");
 checking.pending=false;checking.update.ok=checking.update.updateAvailable=true;checking.update.expectedSha256=std::string(64,'a');checking.update.latestVersion="v9.9.9";
 show();show();Check(found.navigation.Focus()=="install","A newly found update did not take the highlight");
 show(MenuInput::Down);show();Check(found.navigation.Focus()=="check","Moving off the found update did not work");
 show();show();Check(found.navigation.Focus()=="check","The found update took the highlight back after the player moved");
 checking.update.latestVersion="v9.9.10";show();show();Check(found.navigation.Focus()=="install","A newer update did not take the highlight");
 SetMenuEntriesProbe({});
}
void AppearanceGalleries(){
 using namespace sf4e;Harness h;FighterSelector selector;selection::Pick pick;
 selection::Availability available;available.ready=true;available.costumes=3;available.colors[0]=available.colors[1]=5;available.personalActions=1;
 auto frame=[&](unsigned buttons=0,bool editable=true){SetMenuInput({buttons,0});ImGui::NewFrame();
  ImGui::Begin("Gallery test");const bool changed=selector.Draw(pick,false,nullptr,[&](int){return available;},nullptr,editable);ImGui::End();ImGui::Render();return changed;};
 auto press=[&](unsigned buttons,bool editable=true){frame(0,editable);const bool changed=frame(buttons,editable);frame(0,editable);return changed;};
 selector.Navigation().Push("costumes");frame();frame();
 press(MenuInput::Right);Check(pick.costume==0,"Gallery focus committed costume");
 Check(press(MenuInput::Select)&&pick.costume==1,"Costume card did not save");
 Check(selector.Navigation().Screen()=="colors","Saving a costume did not go on to its colors");
 selector.Navigation().Return();selector.Navigation().Push("colors");frame();frame();
 press(MenuInput::Right);Check(pick.color==0,"Gallery focus committed color");
 Check(press(MenuInput::Select)&&pick.color==2,"Color gallery ignored native availability gaps");
 Check(selector.Navigation().Screen()=="home","Saving a color did not return to the selector's home");
 selector.Navigation().Push("colors");frame(0,false);frame(0,false);
 press(MenuInput::Left,false);press(MenuInput::Select,false);Check(pick.color==2,"Locked gallery saved a choice");
 available.colors[1]=0;selector.Navigation().Return();selector.Navigation().Push("costumes");frame(0,false);
 // Missing palette data must not dereference an empty preview list.
 frame(0,false);
 available.colors[1]=5;selector.Navigation().Return();selector.Navigation().Push("ultra");frame();frame();
 std::vector<MenuEntry> ultras;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){ultras=rows;});
 frame();
 // The Ultras are photo cards; Select saves one and returns to the selector's home.
 selector.Navigation().Focus("ultra-1",ultras);frame();Check(pick.ultra==0,"Ultra focus committed a choice");
 Check(press(MenuInput::Select)&&pick.ultra==1&&selector.Navigation().Screen()=="home","Ultra II did not save and return");
 selector.Navigation().Push("ultra");frame();frame();
 Check(pick.ultra==1&&ultras.size()>=2&&ultras[1].value=="SAVED"&&ultras[0].value.empty(),"Saved Ultra has no persistent selection marker separate from focus");
 Dimps::GameEvents::VsMode::ConfirmedCharaConditions native{};
 selection::ToNative(pick,native);pick=selection::FromNative(native);frame();
 Check(pick.ultra==1&&ultras[1].value=="SAVED","Native selection round trip lost Ultra II");
 selector.Navigation().Focus("ultra-0",ultras);press(MenuInput::Select,false);Check(pick.ultra==1,"Locked Ultra selection changed");
 selector.Navigation().Focus("ultra-2",ultras);press(MenuInput::Select);
 Check(pick.ultra==2&&selector.Navigation().Screen()=="home","Ultra Double did not save");
 selector.Navigation().Push("ultra");frame();frame();
 Check(pick.ultra==2&&ultras[2].value=="SAVED","Reopening Ultra lost saved selection");
 // On the home page Left and Right change the Ultra in place, and Select
 // still opens the cards.
 selector.Navigation().Home();frame();selector.Navigation().Focus("ultra",ultras);frame();
 Check(press(MenuInput::Left)&&pick.ultra==1&&selector.Navigation().Screen()=="home","Left on Ultra Combo did not step back to Ultra II");
 Check(!press(MenuInput::Left,false)&&pick.ultra==1,"Locked Ultra Combo row still steps");
 press(MenuInput::Select);Check(selector.Navigation().Screen()=="ultra","Select on Ultra Combo did not open the cards");
 // Picking a fighter goes on to its Ultra, focused on the one it has; Back
 // there keeps the new fighter and returns to the roster.
 selector.Navigation().Home();selector.Navigation().Push("roster");frame();frame();
 selector.Navigation().Focus("fighter-5",ultras);press(MenuInput::Select);
 Check(pick.fighter==5&&selector.Navigation().Screen()=="ultra"&&selector.Navigation().Focus()=="ultra-1","Picking a fighter did not go on to its Ultra");
 press(MenuInput::Back);Check(pick.fighter==5&&pick.ultra==1&&selector.Navigation().Screen()=="roster","Back from the Ultra step lost the fighter");
 // A fighter with a single Ultra in its edition skips the step.
 auto single=[&](unsigned buttons){SetMenuInput({buttons,0});ImGui::NewFrame();ImGui::Begin("Gallery test");
  selector.Draw(pick,true,nullptr,[&](int){return available;},nullptr,true);ImGui::End();ImGui::Render();};
 pick.fighter=0;pick.edition=13;pick.ultra=0;single(0);single(0);
 Check(selection::AllowedUltras(0,13).size()==1,"Ryu's SFIV edition should have one Ultra");
 selector.Navigation().Focus("fighter-0",ultras);single(0);single(MenuInput::Select);single(0);
 Check(pick.edition==13&&selector.Navigation().Screen()=="home","A single-Ultra fighter still asked for its Ultra");
 // The step follows the new fighter's own saved pick, which the caller
 // restores after the pick: from Ryu on SFIV to Zangief saved on Ultra, the
 // step shows; from there to a fighter saved on SFIV, it does not.
 Check(selection::AllowedUltras(5,13).size()==1,"Zangief's SFIV edition should have one Ultra");
 selector.Navigation().Home();selector.Navigation().Push("roster");single(0);single(0);
 selector.Navigation().Focus("fighter-5",ultras);single(0);single(MenuInput::Select);
 pick.edition=14;single(0);single(0);
 Check(pick.fighter==5&&selector.Navigation().Screen()=="ultra","A fighter saved on the Ultra edition skipped its Ultra");
 selector.Navigation().Home();selector.Navigation().Push("roster");single(0);single(0);
 selector.Navigation().Focus("fighter-0",ultras);single(0);single(MenuInput::Select);
 pick.edition=13;single(0);single(0);
 Check(pick.fighter==0&&selector.Navigation().Screen()=="home","A fighter saved on a single-Ultra edition still asked for its Ultra");
 SetMenuEntriesProbe({});
}
