#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include <algorithm>
#include <iterator>
// V2: the right mouse button is Back, and closes an open value panel first.
void RightClickBack(){
 using namespace sf4e;
 Harness h;h.Frame();
 auto& io=ImGui::GetIO();
 const auto rightClick=[&]{io.AddMouseButtonEvent(1,true);h.Frame();io.AddMouseButtonEvent(1,false);h.Frame();h.Frame();};
 h.Screen("replays");rightClick();
 Check(h.shell.Navigation().Screen()=="home","A right click in V2 did not go back");
}
// V2: Right on Home's list reaches the player card beside it, Left goes back.
void HomeCardRight(){
 using namespace sf4e;
 Harness h;h.Frame();
 h.Screen("home");h.FocusOn("settings");h.Press(MenuInput::Right);
 Check(h.shell.Navigation().Focus()=="awards","Right on Home did not reach the player card");
 h.Press(MenuInput::Left);
 Check(h.shell.Navigation().Focus()=="online","Left on the player card did not go back to the top of the list");
}
// V2: a member's actions open at their row; Kick still asks first, on the member's page.
void MemberPopout(){
 using namespace sf4e;
 Harness h;h.Frame();
 JoinedRoom(h,10);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;h.view.room.capacity=8;
 for(room::MemberId id:{1,2}){room::Member m;m.id=id;m.name=id==1?"Local":"Other";h.view.room.members.push_back(m);}
 h.Screen("room");h.FocusOn("member-2");h.Press(MenuInput::Select);
 Check(h.shell.Navigation().Choosing()&&h.shell.Navigation().Screen()=="room","A member's Select did not open their actions in place");
 for(int i=0;i<2;++i)h.Press(MenuInput::Down);
 h.Press(MenuInput::Select);h.Frame(0,2);
 Check(h.shell.Navigation().Screen()=="room-member"&&h.shell.Navigation().Confirming(),"Kick from the popout did not ask first");
}
// The Members panel's Hide folds it to its heading and saves it, in the room's
// own request (CopyRoomSettings); Show on that heading unfolds it again, and More… can too.
void RoomPartsHide(){
 using namespace sf4e;
 Harness h;h.Frame();
 JoinedRoom(h,10);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;h.view.room.host=1;h.view.room.capacity=8;
 for(room::MemberId id:{1,2}){room::Member m;m.id=id;m.name=id==1?"Local":"Other";h.view.room.members.push_back(m);}
 h.Screen("room");h.Choose("room-hide-members");h.Wait(1);h.Frame();
 const auto saved=std::find_if(h.actions.rbegin(),h.actions.rend(),[](const ShellAction& a){return a.roomSettings;});
 Check(saved!=h.actions.rend()&&!saved->preferences.roomShowMembers&&saved->preferences.roomShowChat,"Hide on the Members panel did not save it hidden");
 h.view.preferences=saved->preferences;h.Frame();
 bool gone=false;try{h.FocusOn("member-2");}catch(const std::runtime_error&){gone=true;}
 Check(gone,"A hidden Members panel's rows are still on the board");
 h.Choose("room-show-members");h.Wait(1);h.Frame();
 const auto shown=std::find_if(h.actions.rbegin(),h.actions.rend(),[](const ShellAction& a){return a.roomSettings;});
 Check(shown!=h.actions.rend()&&shown->preferences.roomShowMembers,"Show on the folded Members panel did not unfold it");
 h.view.preferences=shown->preferences;
 h.Frame();
 h.Choose("room-hide-members");h.Wait(1);h.Frame();
 h.Choose("room-more");
 Check(h.shell.Navigation().Choosing(),"More… did not open");
}
// A keyboard player reaches everything a pad does: F, T and C are X, Y and
// View, Escape always goes back, and Delete leaves the seat that B leaves.
void KeyboardJourneys(){
 using namespace sf4e;
 Harness h;h.Frame();
 auto& io=ImGui::GetIO();
 const auto key=[&](ImGuiKey k){h.Frame();io.AddKeyEvent(k,true);h.Frame();io.AddKeyEvent(k,false);h.Frame();};
 SetMenuGlyphs(input::PadKeyboard,0,0);
 JoinedRoom(h,10);h.view.session.generation.room=1;h.view.session.control=netplay::Health::Healthy;
 h.view.room.host=1;h.view.room.name="Keys";h.view.room.revision=3;
 for(auto& table:h.view.room.tables)table.revision=7;
 room::Member local;local.id=1;local.name="Local";local.table=0;local.seat=0;h.view.room.members.push_back(local);
 h.view.room.tables[0].p1=1;
 h.Screen("room");h.FocusOn("table-0");
 h.Frame();
 Check(KeyboardPrompts(),"A keyboard player does not see keyboard prompts");
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
 // F opens fighter select's board at the current fighter; Right and Enter
 // pick the next one there, and Back returns to the room.
 FighterSelector selector;selection::Pick pick;pick.fighter=4;pick.edition=14;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 key(ImGuiKey_F);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="home"&&selector.Navigation().Focus()=="fighter-4",
  "F did not open the board at the current fighter");
 key(ImGuiKey_RightArrow);key(ImGuiKey_Enter);
 Check(pick.fighter==DisplayedAfter(4)&&h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="home","Picking a fighter did not stay on the board");
 key(ImGuiKey_Backspace);
 Check(h.shell.Navigation().Screen()=="room","Back from the board did not return to the room");
 // Select on the table page's Ultra or Appearance opens the board on that row,
 // and Back returns to the table page.
 selection::Availability available;available.ready=true;available.costumes=3;available.colors[0]=available.colors[1]=5;available.personalActions=1;
 h.selection=[&]{selector.Draw(pick,false,nullptr,[&](int){return available;},nullptr,true);};
 h.view.ultraSteps=true;h.view.ultraName="Ultra II";key(ImGuiKey_T);h.FocusOn("ultra");key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="home"&&selector.Navigation().Focus()=="ultra",
  "Select on the table's Ultra did not open the board on its Ultra");
 key(ImGuiKey_Backspace);Check(h.shell.Navigation().Screen()=="room-table","Back from the board did not return to the table page");
 h.view.appearanceName="Original / Color 1";h.FocusOn("appearance");key(ImGuiKey_Enter);
 Check(h.shell.Navigation().Screen()=="selection"&&selector.Navigation().Screen()=="home"&&selector.Navigation().Focus()=="costume",
  "Select on the table's Appearance did not open the board on its costume");
 key(ImGuiKey_Backspace);Check(h.shell.Navigation().Screen()=="room-table","Back from the board did not return to the table page");
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
 h.view.preferences.backgroundPlay=true;h.Frame(); // Off by default; the click below must change a saved value.
 h.Screen("player");h.FocusOn("background-play");ImVec2 leftArrow,otherRow;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){
  if(std::strcmp(id,"background-play")==0)leftArrow=ImVec2(min.x+(max.x-min.x)*.75f,(min.y+max.y)*.5f);
  if(std::strcmp(id,"button-prompts")==0)otherRow=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);
 });h.Frame();SetMenuCardProbe({});
 auto& io=ImGui::GetIO();io.AddMousePosEvent(otherRow.x,otherRow.y);h.Frame();
 Check(h.shell.Navigation().Focus()=="background-play","Mouse hover stole controller focus");
 io.AddMousePosEvent(leftArrow.x,leftArrow.y);h.Frame();io.AddMouseButtonEvent(0,true);h.Frame();
 io.AddMouseButtonEvent(0,false);h.Frame(0,40);
 Check(!h.actions.empty()&&h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.backgroundPlay,"Visible value arrows did not adjust on click");
 Check(status=="Saving...","Queued settings falsely reported Saved before acknowledgement");
 h.view.preferences=h.actions.back().preferences;h.Frame();Check(status=="Saved","Acknowledged settings did not report Saved");h.Press(MenuInput::Down);
 Check(h.shell.Navigation().Focus()=="lower-input-lag","Controller did not move to the row following the mouse-selected row");
 // Gameplay defaults step through 0 frames: Left from a chosen 1 saves 0, and Left from 0 goes to Auto, keeping 0.
 h.view.preferences.inputDelay=1;h.view.preferences.autoInputDelay=false;h.Screen("tab-matches/defaults");h.FocusOn("delay");const auto delaySaves=h.actions.size();
 h.Press(MenuInput::Left);h.Frame(0,40);
 Check(h.actions.size()>delaySaves&&!h.actions.back().preferences.autoInputDelay&&h.actions.back().preferences.inputDelay==0,
  "Gameplay defaults did not offer 0 frames of input delay");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Press(MenuInput::Left);h.Frame(0,40);
 Check(h.actions.back().preferences.autoInputDelay&&h.actions.back().preferences.inputDelay==0,"Left from 0 frames did not choose Auto");
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
 // The channel row: Left and Right pick without saving; Select asks first,
 // with Cancel highlighted, and only a confirmed pick is answered as Channel.
 GameMenu channels;channels.navigation=RecoveryNavigation(true);channels.navigation.Prefer("channel");
 sf4e::platform::ServiceSnapshot saved;saved.channel=sf4e::launcher::UpdateChannel::Beta;ChannelPick pick;
 const auto pickFrame=[&](unsigned held=0){SetMenuInput({held,0});ImGui::NewFrame();const auto choice=DrawRecoveryMenu(channels,saved,"",true,Tone::Error,false,true,&pick);ImGui::Render();return choice;};
 const auto press=[&](unsigned held){const auto choice=pickFrame(held);pickFrame();return choice;};
 const auto channelRow=[&]()->const MenuEntry*{for(const auto& e:updateRows)if(e.id=="channel")return &e;return nullptr;};
 pickFrame();pickFrame();Check(channels.navigation.Focus()=="channel","The channel row did not take the highlight");
 Check(channelRow()&&channelRow()->adjustable&&MenuSelectOpens(*channelRow())==SelectOpens::Nothing,"The saved channel's row offered Select");
 press(MenuInput::Left);
 Check(pick.Shown(saved.channel)==sf4e::launcher::UpdateChannel::Stable&&saved.channel==sf4e::launcher::UpdateChannel::Beta,
  "Left from Beta did not pick Stable, or passed through Nightly");
 Check(channelRow()&&channelRow()->value==sf4e::loc::T("updates.channel.stable")&&
  channelRow()->detail.find(sf4e::loc::Tf("updates.channel.going_back",sf4e::loc::T("updates.channel.prerelease"),sf4e::loc::T("updates.channel.stable")))!=std::string::npos,
  "Picking Stable from Beta did not say what going back means");
 press(MenuInput::Select);
 Check(channels.navigation.Confirming()&&!channels.navigation.ConfirmSelected(),"The channel switch did not ask first, on Cancel");
 Check(press(MenuInput::Select)==RecoveryChoice::None&&!channels.navigation.Confirming(),"Cancel switched the channel");
 Check(pick.Shown(saved.channel)==sf4e::launcher::UpdateChannel::Stable,"Cancel dropped the pick");
 press(MenuInput::Right);press(MenuInput::Right);
 Check(pick.Shown(saved.channel)==sf4e::launcher::UpdateChannel::Nightly,"Right did not reach Nightly");
 Check(channelRow()->detail.find(sf4e::loc::T("updates.channel.nightly_detail"))!=std::string::npos,"The Nightly pick did not show its warning");
 press(MenuInput::Right);Check(pick.Shown(saved.channel)==sf4e::launcher::UpdateChannel::Nightly,"Right went past Nightly");
 press(MenuInput::Select);press(MenuInput::Right);
 Check(channels.navigation.Confirming()&&channels.navigation.ConfirmSelected(),"Right did not move to the confirm button");
 Check(press(MenuInput::Select)==RecoveryChoice::Channel&&pick.picked&&*pick.picked==sf4e::launcher::UpdateChannel::Nightly,
  "The confirmed pick was not answered as Channel");
 SetMenuEntriesProbe({});
}
// The board's Costume, Color and Ultra rows: Left and Right step and save,
// Select opens a row's values in a panel, and a locked pick steps nothing.
void AppearanceGalleries(){
 using namespace sf4e;Harness h;FighterSelector selector;selection::Pick pick;
 // Three costumes, each with colors 1 and 3 only.
 selection::Availability available;available.ready=true;available.costumes=7;available.colors[0]=available.colors[1]=available.colors[2]=5;available.personalActions=1;
 std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r){rows=r;});
 auto frame=[&](unsigned buttons=0,bool editable=true,bool editions=false){SetMenuInput({buttons,0});ImGui::NewFrame();
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);ImGui::Begin("Board test");const bool changed=selector.Draw(pick,editions,nullptr,[&](int){return available;},nullptr,editable);ImGui::End();ImGui::Render();return changed;};
 auto press=[&](unsigned buttons,bool editable=true,bool editions=false){frame(0,editable,editions);const bool changed=frame(buttons,editable,editions);frame(0,editable,editions);return changed;};
 auto row=[&](const char* id){const auto it=std::find_if(rows.begin(),rows.end(),[&](const MenuEntry& e){return e.id==id;});
  if(it==rows.end())throw std::runtime_error(std::string("The board has no row ")+id);return *it;};
 auto focus=[&](const char* id,bool editable=true,bool editions=false){frame(0,editable,editions);selector.Navigation().Focus(id,rows);frame(0,editable,editions);};
 frame();frame();
 Check(selector.Navigation().Screen()=="home","Fighter select did not open on its board");
 // The pad moves by where the board draws its rows: Down keeps the grid's
 // column, Right from the grid's right edge reaches the rows beside it where
 // they are level (and stays where none is), and Left from Ready reaches the grid.
 const auto cell=[](int i){return "fighter-"+std::to_string(selection::RosterDisplayOrder[i]);};
 focus(cell(1).c_str());press(MenuInput::Down);
 Check(selector.Navigation().Focus()==cell(9),"Down on the grid left its column");
 bool besideGrid=false;
 for(int i=7;i<selection::FighterCount;i+=8){
  focus(cell(i).c_str());press(MenuInput::Right);
  const auto& at=selector.Navigation().Focus();
  Check(at==cell(i)||at.rfind("fighter-",0)!=0,"Right from the grid's right edge went to another fighter");
  besideGrid=besideGrid||at=="costume";
 }
 Check(besideGrid,"Right from the grid never reached the costume row");
 focus("ready");press(MenuInput::Left);
 Check(selector.Navigation().Focus().rfind("fighter-",0)==0,"Left from Ready did not reach the grid");
 pick=selection::Pick{};frame();frame();
 // Costume: Right saves the next one; Select opens every costume, and a pick there saves it.
 focus("costume");Check(pick.costume==0,"Focusing the costume row changed the costume");
 Check(press(MenuInput::Right)&&pick.costume==1&&selector.Navigation().Screen()=="home","Right on the costume row did not save the next costume");
 Check(row("costume").choices.size()==3,"The costume row does not offer every owned costume");
 press(MenuInput::Select);Check(selector.Navigation().Choosing(),"Select on the costume row did not open its values");
 press(MenuInput::Right);Check(pick.costume==1,"Moving in the costume panel saved a costume");
 press(MenuInput::Select);
 Check(pick.costume==2&&!selector.Navigation().Choosing()&&selector.Navigation().Screen()=="home","A costume picked in the panel did not save");
 focus("costume");press(MenuInput::Left);Check(pick.costume==1,"Left on the costume row did not step back");
 // Color: the steps skip the colors the game says this costume lacks (bits 0 and 2 only).
 focus("color");Check(press(MenuInput::Right)&&pick.color==2,"The color row ignored native availability gaps");
 press(MenuInput::Left,false);press(MenuInput::Right,false);Check(pick.color==2,"A locked color row saved a step");
 // Missing palette data must not dereference an empty preview list.
 available.colors[1]=0;frame(0,false);frame(0,false);available.colors[1]=5;
 // Ultra: the row steps through the fighter's Ultras; Double is the last.
 pick.ultra=0;focus("ultra");
 Check(press(MenuInput::Right)&&pick.ultra==1&&row("ultra").value==ShortUltraLabel(1),"Right on the Ultra row did not save Ultra II");
 Dimps::GameEvents::VsMode::ConfirmedCharaConditions native{};
 selection::ToNative(pick,native);pick=selection::FromNative(native);frame();
 Check(pick.ultra==1&&row("ultra").value==ShortUltraLabel(1),"Native selection round trip lost Ultra II");
 press(MenuInput::Right,false);Check(pick.ultra==1,"A locked Ultra row saved a step");
 press(MenuInput::Right);Check(pick.ultra==2&&row("ultra").value==ShortUltraLabel(2),"Ultra Double did not save");
 Check(press(MenuInput::Left)&&pick.ultra==1&&selector.Navigation().Screen()=="home","Left on Ultra Combo did not step back to Ultra II");
 Check(!press(MenuInput::Left,false)&&pick.ultra==1,"Locked Ultra Combo row still steps");
 press(MenuInput::Select);Check(selector.Navigation().Choosing()&&selector.Navigation().Screen()=="home","Select on Ultra Combo did not open its values");
 press(MenuInput::Back);
 // Picking a fighter keeps the board, Ultra row and all: no separate Ultra step.
 focus("fighter-5");press(MenuInput::Select);
 Check(pick.fighter==5&&selector.Navigation().Screen()=="home"&&!selector.Navigation().Choosing(),"Picking a fighter left the board");
 // The Ultra row follows the pick as the caller restores it for the new
 // fighter: on a single-Ultra edition it has nothing to step.
 Check(selection::AllowedUltras(0,13).size()==1&&selection::AllowedUltras(5,13).size()==1,"Ryu's and Zangief's SFIV editions should have one Ultra");
 pick.fighter=0;pick.edition=13;pick.ultra=0;frame(0,true,true);frame(0,true,true);
 Check(!row("ultra").enabled,"A single-Ultra fighter's Ultra row still steps");
 focus("fighter-5",true,true);press(MenuInput::Select,true,true);pick.edition=14;frame(0,true,true);
 Check(pick.fighter==5&&row("ultra").enabled,"A fighter saved on the Ultra edition lost its Ultra row");
 focus("fighter-0",true,true);press(MenuInput::Select,true,true);pick.edition=13;frame(0,true,true);
 Check(pick.fighter==0&&!row("ultra").enabled,"A fighter saved on a single-Ultra edition still offers its Ultras");
 SetMenuEntriesProbe({});
}
