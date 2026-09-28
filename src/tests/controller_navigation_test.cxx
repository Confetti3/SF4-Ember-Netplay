#include "../ui/ApplicationShell.hxx"
#include "../ui/ControllerNavigation.hxx"
#include "../ui/Theme.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/FighterSelector.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../ui/MenuGlyphs.hxx"
#include "../ui/MenuPresentation.hxx"
#include "../ui/RecoveryMenu.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../common/MenuInputCapture.hxx"
#include "../netplay/ProfileRecordJson.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "imgui_test_support.hxx"
#include <imgui.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
using namespace sf4e::ui;
using Button=ControllerSample;
using Kind=sf4e::netplay::CommandKind;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
alignas(4) std::array<BYTE, 0x100> nativeSystem{};
alignas(4) std::array<BYTE, 0x1fa4> nativeDevices{};
alignas(4) std::array<BYTE, 12 * 0x27c> nativeBindings{};
Dimps::Pad::System* SystemStub() { return reinterpret_cast<Dimps::Pad::System*>(nativeSystem.data()); }
Dimps::Pad::System_XInput* DevicesStub() { return reinterpret_cast<Dimps::Pad::System_XInput*>(nativeDevices.data()); }
void NativeReader() {
    Dimps::Pad::System::staticMethods.GetSingleton = SystemStub;
    Dimps::Pad::System_XInput::staticMethods.GetSingleton = DevicesStub;
    *reinterpret_cast<int*>(nativeDevices.data() + 0xd00) = 12;
    *reinterpret_cast<BYTE**>(nativeSystem.data() + 0xd4) = nativeBindings.data();
    *reinterpret_cast<BYTE**>(nativeSystem.data() + 0xd8) = nativeBindings.data() + nativeBindings.size();
    for (int index : {2, 7}) {
        auto* entry = nativeDevices.data() + 0xd14 + index * 0x100;
        *reinterpret_cast<int*>(entry) = 1;
        *reinterpret_cast<unsigned*>(entry + 0xc) = (1u << 31) | 1;
        auto* bindings = reinterpret_cast<unsigned*>(nativeBindings.data() + index * 0x27c + 0x17c);
        bindings[0] = 0x10; bindings[31] = 0x4; // remapped LP and left
        const auto systemBefore = nativeSystem;
        const auto devicesBefore = nativeDevices;
        const auto bindingsBefore = nativeBindings;
        unsigned held = 0;
        Check(Dimps::Pad::ReadController(index < 4 ? 3 : 4, index, held) && held == 0x14, "Native cached/remapped read failed");
        Check(nativeSystem == systemBefore && nativeDevices == devicesBefore && nativeBindings == bindingsBefore,
              "Reading a controller changed native state");
        *reinterpret_cast<int*>(nativeSystem.data() + 0xfc) = 1;
        Check(!Dimps::Pad::ReadController(index < 4 ? 3 : 4, index, held) && held == 0, "Native input suspension ignored");
        *reinterpret_cast<int*>(nativeSystem.data() + 0xfc) = 0;
        *reinterpret_cast<int*>(entry) = 0;
        Check(!Dimps::Pad::ReadController(index < 4 ? 3 : 4, index, held) && held == 0, "Disconnect returned stale buttons");
    }
    unsigned held = 99;
    Check(!Dimps::Pad::ReadController(3, -1, held) && held == 0, "Negative device accepted");
    Check(!Dimps::Pad::ReadController(3, 12, held), "Out-of-range device accepted");
    Check(!Dimps::Pad::ReadController(1, 0, held), "Keyboard duplicated as gamepad");
    Check(!Dimps::Pad::ReadController(3, 7, held), "Wrong provider identity accepted");
    Check(ControllerButtons(0x1) == Button::Up && ControllerButtons(0x2) == Button::Down &&
          ControllerButtons(0x4) == Button::Left && ControllerButtons(0x8) == Button::Right, "Native direction bits incorrect");
    Check(ControllerButtons(0x10) == Button::Confirm && ControllerButtons(0x1000) == (Button::Confirm | Button::Menu) &&
          ControllerButtons(0x40) == Button::Back && ControllerButtons(0x2000) == Button::Back, "Native action bits incorrect");
    Check(ControllerButtons(0x20 | 0x80 | 0x400 | 0x800) == 0, "Unrelated attack became UI action");
    Check(ControllerButtons(0x40,3,0x40000)==Button::Confirm,"Physical A must select even when bound to LK");
    Check(ControllerButtons(0x20,3,0x20000)==Button::Back,"Physical B must return even when bound to MP");
    Check(ControllerButtons(0x10,3,0x80000)==Button::Fighter,"Physical X must be the fighter shortcut, not select, when bound to LP");
    Check(ControllerButtons(0,3,0x10000)==Button::Options&&ControllerButtons(0,3,0x100)==Button::Chat,"Physical Y and View must be the options and chat shortcuts");
    Check(ControllerButtons(0x80|0x400,-1,0)==0,"DirectInput attack buttons must not become shortcuts");
    Check(ControllerButtons(0x1000,3,0x200)==Button::Menu,"Start must not activate a visible menu row");
    Check(ControllerButtons(0x9,3,0)==(Button::Up|Button::Right),"Menu directions changed");
}


void NavigationModel() {
 MenuNavigation nav;double time=0;std::vector<MenuEntry> rows={Row("a","A","first"),Value("b","B","1","value"),Row("disabled","Unavailable","Reason",false),ConfirmRow("delete","Delete","destructive"),TextRow("text","Name","Saved",12)};
 auto frame=[&](unsigned held){time+=.016;return nav.Update({held,time},rows);};
 auto press=[&](unsigned held){frame(0);return frame(held);};
 frame(0);Check(nav.Focus()=="a","Initial focus");
 press(MenuInput::Up);Check(nav.Focus()=="a","List must clamp top");
 press(MenuInput::Down);Check(nav.Focus()=="b","List next entry");
 auto a=press(MenuInput::Right);Check(a.kind==MenuAction::Adjust&&a.delta==1,"Value adjustment");
 nav.Push("child");frame(0);press(MenuInput::Down);nav.Scroll()=41;
 press(MenuInput::Back);Check(nav.Screen()=="home","Back one level");
 frame(0);Check(nav.Focus()=="b","Parent focus restored");
 nav.Push("child");frame(0);Check(nav.Focus()=="b"&&nav.Scroll()==41,"Child focus and scroll restored");
 press(MenuInput::Down);Check(nav.Focus()=="disabled","Disabled entries focusable");
 Check(press(MenuInput::Select).kind==MenuAction::None,"Disabled action submitted");
 press(MenuInput::Down);press(MenuInput::Select);Check(nav.Confirming()&&!nav.ConfirmSelected(),"Confirmation must default Cancel");
 Check(press(MenuInput::Select).kind==MenuAction::None&&!nav.Confirming(),"Cancel confirmation");
 press(MenuInput::Select);press(MenuInput::Right);a=press(MenuInput::Select);Check(a.id=="delete"&&a.kind==MenuAction::Activate,"Explicit confirmation");
 Check(frame(MenuInput::Select).kind==MenuAction::None,"Held destructive action repeated");
 press(MenuInput::Down);press(MenuInput::Select);Check(nav.Editing(),"Explicit text editing");
 nav.Draft("Draft");press(MenuInput::Up);Check(nav.Editing()&&nav.Draft()=="Draft","Controller altered draft");
 press(MenuInput::Back);Check(!nav.Editing()&&nav.Screen()=="child","Text Back did more than cancel");
 press(MenuInput::Select);nav.Draft("Accepted");frame(0);a=nav.Update({0,time,true},rows);Check(a.kind==MenuAction::TextAccepted&&a.text=="Accepted","Accept draft");
 nav.Focus("delete",rows);press(MenuInput::Select);rows.erase(rows.begin()+3);frame(0);Check(!nav.Confirming()&&nav.Focus()=="text","Removed dialog target or nearest focus");
 rows.insert(rows.begin(),Row("new","New",""));frame(0);Check(nav.Focus()=="text","Insertion stole stable identity");
 nav.Home();rows={Row("0","0",""),Row("1","1",""),Row("2","2",""),Row("3","3",""),Row("4","4","")};
 nav.Reconcile(rows);nav.Focus("1",rows);nav.Update({0,time},rows,3);
 nav.Update({MenuInput::Up,time+.1},rows,3);Check(nav.Focus()=="1","Grid top edge moved sideways");
 nav.Update({0,time+.2},rows,3);nav.Update({MenuInput::Right,time+.3},rows,3);Check(nav.Focus()=="2","Grid right");
 nav.Update({0,time+.4},rows,3);nav.Update({MenuInput::Right,time+.5},rows,3);Check(nav.Focus()=="2","Grid wrapped");
 nav.Update({MenuInput::Down,time+.6},rows,3);Check(nav.Focus()=="4","Incomplete grid row");
 // A wide entry and those after it are the grid's footer, one row each, under
 // the cards: Down reaches it from every card of an incomplete last row, and
 // Up returns to the cards.
 {auto grid=rows;grid.push_back(Row("retry","Retry",""));grid.back().wide=true;grid.push_back(Row("after","After",""));
  const auto step=[&](const char* from,unsigned held){nav.Focus(from,grid);nav.Update({0,time},grid,3);nav.Update({held,time+.01},grid,3);nav.Update({0,time+.02},grid,3);return nav.Focus();};
  Check(MenuGridCells(grid)==5,"The grid footer does not start at the first wide entry");
  Check(step("3",MenuInput::Down)=="retry"&&step("4",MenuInput::Down)=="retry","Down from the last cards did not reach the footer");
  Check(step("1",MenuInput::Down)=="4","Down above an incomplete row no longer reaches its last card");
  Check(step("4",MenuInput::Right)=="4","Right from the last card reached the footer");
  Check(step("retry",MenuInput::Up)=="4"&&step("retry",MenuInput::Down)=="after"&&step("after",MenuInput::Up)=="retry","The footer rows do not step by one");
  Check(step("retry",MenuInput::Right)=="retry","Right in the footer moved");}
 nav.Update({0,time+1},rows);nav.Focus("0",rows);
 nav.Update({MenuInput::Down,time+1.1},rows);Check(nav.Focus()=="1","Immediate direction press");
 nav.Update({MenuInput::Down,time+1.2},rows);Check(nav.Focus()=="1","Repeat too early");
 nav.Update({MenuInput::Down,time+1.5},rows);Check(nav.Focus()=="2","Direction did not repeat");
 nav.NeutralGate();nav.Update({MenuInput::Select,time+2},rows);Check(nav.Update({0,time+3},rows).kind==MenuAction::None,"Neutral gate");
 // A choice returns the option the player saw, by id. One whose options
 // change meaning closes rather than send something never picked, and Back
 // cancels without choosing either side.
 nav.Home();MenuEntry seat=Row("seat","Seat","");seat.choices={{"sit-p1","P1"},{"sit-p2","P2"}};rows={seat};
 nav.Reconcile(rows);frame(0);press(MenuInput::Select);Check(nav.Choosing()&&nav.ChoiceIndex()==0,"Choice did not open on its left option");
 press(MenuInput::Right);a=press(MenuInput::Select);
 Check(a.kind==MenuAction::Chosen&&a.id=="seat"&&a.text=="sit-p2"&&!nav.Choosing(),"Choice did not return the picked option");
 press(MenuInput::Select);a=press(MenuInput::Back);Check(a.kind==MenuAction::None&&!nav.Choosing(),"Back did more than cancel the choice");
 press(MenuInput::Select);Check(nav.Confirm(true,rows).kind==MenuAction::None&&nav.Choosing(),"Confirm answered a choice");
 press(MenuInput::Right);rows[0].choices={{"sit-p2","P2"},{"watch","Watch"}};
 a=press(MenuInput::Select);Check(a.kind==MenuAction::None&&nav.Choosing()&&nav.ChoiceIndex()==0,"Changed choice sent an option the player never saw");
 // Right stops at the last option; Left at the first.
 rows[0].choices.push_back({"options","Options"});frame(0);nav.Cancel();press(MenuInput::Select);
 for(int i=0;i<4;++i)press(MenuInput::Right);Check(nav.ChoiceIndex()==2,"Right ran past the last option");
 a=press(MenuInput::Select);Check(a.kind==MenuAction::Chosen&&a.text=="options","The third option was not chosen");
 nav.Cancel();
}
void NativeCapture() {
 Check(std::strcmp(PhysicalGlyph(3,0x80000,"LP"),"X")==0&&std::strcmp(PhysicalGlyph(3,0x40000,"LK"),"A")==0,"Physical glyphs ignore native mapping");
 Check(std::strcmp(PhysicalGlyph(4,0x80000,"LP"),"LP")==0,"DirectInput guessed an Xbox glyph");
 unsigned char caches[0x100];std::memset(caches,0x5a,sizeof(caches));
 Check(sf4e::input::NativeMenuHeld(caches)==0x5a5a5a5a,"Native cache offsets");
 sf4e::input::ClearNativeMenuInputs(caches);
 for(unsigned i=0;i<sizeof(caches);++i){const bool input=(i>=0x18&&i<0x2c)||(i>=0x68&&i<0x7c);
  Check(caches[i]==(input?0:0x5a),"Native suppression corrupted assignment or missed an input cache");}
 using sf4e::input::MenuContext;using sf4e::input::ControllerMenuAvailable;
 Check(ControllerMenuAvailable(MenuContext::MainMenu)&&!ControllerMenuAvailable(MenuContext::OfflineTraining)&&
  !ControllerMenuAvailable(MenuContext::Unavailable),"Controller navigation escaped the main-menu context");
 sf4e::input::MenuInputCapture capture;
 Check(!capture.Update(false,16),"Native input suppressed while overlay hidden");
 Check(capture.Update(true,16)&&capture.Update(true,0),"Open overlay not suppressing native input");
 Check(capture.Update(false,16)&&capture.Update(false,16),"Closing leaked held input");
 Check(!capture.Update(false,0)&&!capture.Update(false,16),"New gameplay press blocked after release");
 ControllerNavigation adapter;ControllerSample device{3,2,true,16};
 adapter.Update(device,true,true,true);Check(adapter.Buttons()==0,"Held opening button activated");
 device.buttons=0;adapter.Update(device,true,true,true);
 device.buttons=16;adapter.Update(device,true,true,true);Check(adapter.Buttons()==16,"Assigned device input missing");
 adapter.Update(device,true,true,false);Check(!adapter.Buttons(),"Focus loss not gated");
 adapter.Update(device,true,true,true);Check(!adapter.Buttons(),"Held refocus input activated");
 device.buttons=0;adapter.Update(device,true,true,true);device.buttons=32;
 adapter.Update(device,true,true,true);adapter.Update(device,true,false,true);Check(adapter.MenuGuard(),"Main-menu suppression not drained");
 device.buttons=0;adapter.Update(device,true,false,true);Check(!adapter.MenuGuard(),"Main menu never released");
}
struct Harness {
 ApplicationShell shell;ShellView view;std::vector<ShellAction> actions;bool open=true,accept=true;
 std::function<void()> selection=[]{};
 HeadlessImGui imgui; // last: created after the shell and destroyed before it
 Harness(){
  view.canEditPreferences=view.canOpenRoom=view.helperReady=view.controllerReady=view.canChangeController=true;
 }
 void Frame(unsigned buttons=0,int count=1){
  for(int i=0;i<count;++i){auto& io=ImGui::GetIO();io.DeltaTime=1.f/60;SetMenuInput({buttons,0});ImGui::NewFrame();
   shell.Draw(view,&open,[&](ShellAction a){actions.push_back(a);return accept;},selection);
   ImGui::Render();}
 }
 void Press(unsigned b){Frame();Frame(b);Frame();}
 void FocusOn(const char* id){
  Frame();for(int i=0;i<100&&shell.Navigation().Focus()!=id;++i)Press(MenuInput::Up);
  for(int i=0;i<100&&shell.Navigation().Focus()!=id;++i)Press(MenuInput::Down);
  if(shell.Navigation().Focus()!=id)throw std::runtime_error(std::string("Journey item not reachable: ")+id+" on "+shell.Navigation().Screen()+" focused "+shell.Navigation().Focus());
 }
 void Choose(const char* id){FocusOn(id);Press(MenuInput::Select);}
 void Screen(const char* id){shell.Navigation().Home();if(std::string(id)!="home")shell.Navigation().Push(id);Frame();}
};
void Journeys() {
 using namespace sf4e;
 Harness h;h.Frame();
 // Back names where it goes: out of Ember from an idle Home, and while a
 // controller is being captured, the cancel it is.
 std::set<std::string> legend;
 SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))legend.insert(id+7);});
 SetMenuGlyphs(3,0x40000,0x20000);legend.clear();h.Frame();Check(legend.count("Return to SF4"),"Idle Home's Back does not say it returns to SF4");
 h.Screen("player");h.view.inputCapture=input::Capture::ReleaseAll;legend.clear();h.Frame();
 Check(legend.count("Cancel")&&!legend.count("Back"),"Controller assignment's Back does not say it cancels");
 SetMenuTextProbe({});h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="assignment"&&h.actions.back().inputAction==input::Action::Cancel,"Assignment Back did not only cancel capture");
 h.view.inputCapture=input::Capture::Idle;h.Frame();Check(h.shell.Navigation().Screen()=="player","Assignment lost return destination");
 h.Screen("home");h.Choose("online");Check(h.shell.Navigation().Screen()=="online","Online route");
 h.Choose("create");h.Choose("host");Check(h.actions.back().command.kind==Kind::HostRoom,"Create journey");
 // Opening a room keeps the player on Create with a Cancel; the room screen
 // appears only once the committed snapshot says the room is joined.
 // The command is queued, so a few Idle frames can pass before Opening; the
 // origin survives them, and Back from Home returns to Create, not Join.
 h.Frame(0,5);h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Opening;h.Frame();
 Check(h.shell.Navigation().Screen()=="create","Opening room showed the placeholder room screen");
 h.Screen("home");h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="create","Back from Home while creating did not return to Create");
 // Create's opening says Creating, on its own screen and on Home's status and
 // Online row; Join's says Joining (below).
 std::string openingStatus;std::vector<MenuEntry> openingRows;
 SetMenuStatusProbe([&](const char* status,Tone){openingStatus=status;});
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){openingRows=rows;});
 const auto onlineDetail=[&]{const auto it=std::find_if(openingRows.begin(),openingRows.end(),[](const MenuEntry& e){return e.id=="online";});
  return it==openingRows.end()?std::string():it->detail;};
 h.Frame();Check(openingStatus==loc::T("room.creating_status"),"Creating a room is not reported as creating");
 h.Screen("home");Check(openingStatus==loc::T("room.creating_status")&&onlineDetail()==loc::T("room.creating_status"),
  "Home does not say the room is being created");
 h.Press(MenuInput::Back);
 h.Choose("cancel-open");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.back().command.kind==Kind::LeaveRoom,"Cancel while opening did not leave the room");
 h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;h.view.room.roomEpoch=9;h.view.room.localMember=1;h.Frame();
 Check(h.shell.Navigation().Screen()=="room","Joined room did not open the room screen");
 h.view.session.room=netplay::RoomState::Idle;h.view.session.generation.room=0;h.view.room.roomEpoch=0;h.view.room.localMember=0;h.Frame();h.Screen("home");
 h.Screen("join");h.Choose("invite-text");ImGui::GetIO().AddInputCharactersUTF8("sf4://invitation");h.Frame();
 ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 h.Choose("join-now");Check(h.actions.back().command.kind==Kind::JoinInvite&&h.actions.back().command.invitation=="sf4://invitation","Join draft journey");
 h.Frame(0,5);h.view.session.room=netplay::RoomState::Opening;h.Frame();
 Check(h.shell.Navigation().Screen()=="join"&&openingStatus==loc::T("room.joining_status"),"Joining a room is not reported as joining");
 h.Screen("home");Check(openingStatus==loc::T("room.joining_status")&&onlineDetail()==loc::T("room.joining_status"),
  "Home does not say the room is being joined");
 SetMenuStatusProbe({});SetMenuEntriesProbe({});
 h.view.session.generation.room=1;h.view.session.room=netplay::RoomState::Joined;h.view.session.control=netplay::Health::Healthy;
 h.view.room.roomEpoch=10;h.view.room.localMember=1;h.view.room.host=1;h.view.room.name="Test room";h.view.room.revision=3;
 for(int i=0;i<4;++i){h.view.room.tables[i].id=i;h.view.room.tables[i].revision=7;}
 room::Member local;local.id=1;local.name="Local";h.view.room.members.push_back(local);
 room::Member peer;peer.id=2;peer.name="Peer";h.view.room.members.push_back(peer);
 h.Frame();h.Screen("room");h.Press(MenuInput::Right);Check(h.shell.Navigation().Focus()=="member-1","Right did not enter member pane");
 h.Press(MenuInput::Down);Check(h.shell.Navigation().Focus()=="member-2","Member focus order changed");
 h.view.room.members.pop_back();h.Frame();Check(h.shell.Navigation().Focus()=="room-members","Disappeared member did not choose nearest entry");
 h.view.room.members.push_back(peer);h.Press(MenuInput::Left);h.Screen("room");
 h.view.preferences.mainFighter=8;h.view.selectedFighter=0;
 h.view.room.members[1].mainFighter=9;h.view.room.members[1].fighter=2;
 h.view.room.tables[0].p1=1;h.view.room.tables[0].p2=2;
 std::vector<int> portraits;SetPortraitProbe([&](int fighter,ImVec2,ImVec2){portraits.push_back(fighter);});
 h.Frame();SetPortraitProbe({});
 Check(portraits==std::vector<int>({0,2,8,9}),"Member portraits must use saved mains, not battle fighters");
 h.view.room.tables[0].p1=h.view.room.tables[0].p2=0;
 // A on an empty table opens the seat chooser: P1 left, P2 right, B cancels.
 h.Choose("table-2");Check(h.shell.Navigation().Confirming(),"A on an empty table did not open the seat chooser");
 auto before=h.actions.size();h.Press(MenuInput::Back);
 Check(!h.shell.Navigation().Confirming()&&h.actions.size()==before&&h.shell.Navigation().Screen()=="room","B did not cancel the seat chooser");
 h.Choose("table-2");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.back().roomAction.kind==room::ActionKind::Queue&&h.actions.back().roomAction.table==2&&
  h.actions.back().roomAction.seat==1,"Seat chooser did not take the P2 seat");
 // P1 taken under a chooser highlighting P2: the chooser closes, the A pressed
 // on that same frame does nothing, and the next A offers the new options.
 h.Choose("table-1");h.Press(MenuInput::Right);h.view.room.tables[1].p1=2;
 auto raced=h.actions.size();h.Frame(MenuInput::Select);h.Frame();
 Check(h.actions.size()==raced&&!h.shell.Navigation().Confirming(),"A changed chooser sent an option the player never saw");
 h.Press(MenuInput::Select);h.Press(MenuInput::Select);
 Check(h.actions.size()==raced+1&&h.actions.back().roomAction.kind==room::ActionKind::Queue&&h.actions.back().roomAction.seat==1,
  "The reopened chooser did not offer the open P2 seat first");
 h.view.room.tables[1].p1=0;
 // The mouse picks from the chooser with real buttons: a notice over it
 // blocks them, and a click on another table's card opens that table's
 // chooser even though its game count differs.
 std::map<std::string,ImVec2> centres;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){centres[id]=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);});
 const auto click=[&](const std::string& id){
  h.Frame();const auto at=centres.at(id);auto& io=ImGui::GetIO();io.AddMousePosEvent(at.x,at.y);h.Frame();
  io.AddMouseButtonEvent(0,true);h.Frame();io.AddMouseButtonEvent(0,false);h.Frame();h.Frame();
 };
 h.view.room.tables[3].matchGeneration=5;h.Choose("table-2");h.Press(MenuInput::Back);
 click("table-3");Check(h.shell.Navigation().Choosing()&&h.shell.Navigation().DialogId()=="table-3","Clicking another table did not keep its chooser open");
 h.view.readyFailure="Notice over the chooser.";h.view.readyFailureSequence=7;raced=h.actions.size();
 click("table-3/1");Check(h.actions.size()==raced&&h.shell.Navigation().Choosing(),"A click reached the chooser under a notice");
 h.Press(MenuInput::Select);
 // A cursor resting on P1 does not undo P2 picked on the pad.
 {const auto at=centres.at("table-3/0");ImGui::GetIO().AddMousePosEvent(at.x,at.y);h.Frame();h.Frame();}
 h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==raced+1&&h.actions.back().roomAction.seat==1,"A resting cursor overrode the pad's choice");
 // The header button cancels a choice for a mouse, sending nothing.
 click("table-3");Check(h.shell.Navigation().Choosing(),"Clicking a table did not open its chooser");
 raced=h.actions.size();click("menu-back");
 Check(!h.shell.Navigation().Confirming()&&h.actions.size()==raced&&h.shell.Navigation().Screen()=="room","The header did not cancel the chooser");
 click("table-3");click("table-3/1");
 Check(h.actions.size()==raced+1&&h.actions.back().roomAction.table==3&&h.actions.back().roomAction.seat==1,"Clicking P2 in the chooser did not take it");
 ImGui::GetIO().AddMousePosEvent(-1,-1);
 SetMenuCardProbe({});h.view.room.tables[3].matchGeneration=0;
 // A full table offers the queue or watching instead.
 h.view.room.tables[3].p1=3;h.view.room.tables[3].p2=4;h.view.room.tables[3].phase=room::TablePhase::Playing;
 h.Choose("table-3");h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.back().roomAction.kind==room::ActionKind::Watch&&h.actions.back().roomAction.table==3,"Full-table chooser did not watch");
 h.view.room.tables[3].p1=h.view.room.tables[3].p2=0;h.view.room.tables[3].phase=room::TablePhase::Idle;
 // Y opens the table's options; the list still queues and watches.
 h.Screen("room");h.Choose("table-2");h.Press(MenuInput::Back);h.Press(MenuInput::Options);
 Check(h.shell.Navigation().Screen()=="room-table","Y did not open the table options");
 h.Choose("queue");Check(h.actions.back().roomAction.kind==room::ActionKind::Queue&&h.actions.back().roomAction.table==2,"Table queue journey");
 h.Choose("watch");Check(h.actions.back().roomAction.kind==room::ActionKind::Watch,"Watch journey");
 h.Press(MenuInput::Options);Check(h.shell.Navigation().Screen()=="room","Y again did not return to the board");
 h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room-chat","View did not open chat");
 h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room","View again did not return to the board");
 // X opens the real fighter selector, which hands its shortcuts back: X
 // there returns to the board and View goes on to chat.
 FighterSelector selector;selection::Pick pick;
 h.selection=[&]{selector.Draw(pick,false,nullptr,{},nullptr,true);};h.view.canEditSelection=true;
 h.Press(MenuInput::Fighter);Check(h.shell.Navigation().Screen()=="selection","X did not open fighter selection");
 // The selector names the board as its way back and shows the room's shortcuts.
 Check(EmbeddedReturnContext().exitName=="Room"&&EmbeddedReturnContext().shortcutHints.size()==3,
  "The fighter selector does not know it returns to the room or which shortcuts it forwards");
 h.Press(MenuInput::Fighter);Check(h.shell.Navigation().Screen()=="room","X in fighter selection did not return to the board");
 h.Press(MenuInput::Fighter);h.Press(MenuInput::Chat);Check(h.shell.Navigation().Screen()=="room-chat","View in fighter selection did not open chat");
 h.Press(MenuInput::Chat);h.selection=[]{};h.view.canEditSelection=false;
 // B on the board with no seat goes to Home, and B there returns to the room
 // rather than dropping to the game's own menu.
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="home","B on the board did not reach Home");
 h.Press(MenuInput::Back);Check(h.open&&h.shell.Navigation().Screen()=="room","B at Home left the room for the game menu");
 h.Choose("table-2");h.Press(MenuInput::Back);h.Press(MenuInput::Options);
 h.view.room.members[0].table=2;h.view.room.members[0].seat=0;h.view.room.tables[2].p1=1;h.view.room.tables[2].p2=2;
  h.view.room.tables[2].phase=room::TablePhase::Waiting;h.view.canReady=true;
 std::vector<MenuEntry> tableRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){tableRows=rows;});
 h.view.canReady=false;h.view.canEditSelection=false;h.view.room.tables[2].p2=0;h.Frame();
 const auto row=[&](const char* id)->const MenuEntry&{return *std::find_if(tableRows.begin(),tableRows.end(),[&](const MenuEntry& e){return e.id==id;});};
 Check(row("ready").detail.find("opponent")!=std::string::npos,"Ready does not explain the missing opponent");
 Check(row("selection").detail.find("Unready")==std::string::npos,"Unready instruction shown to an unready player");
 Check(!row("selection").enabled,"Locked fighter change pretends to be available");
 h.view.canEditSelection=true;h.Frame();Check(row("selection").enabled,"Waiting solo player cannot change fighter");
  h.view.room.tables[2].p2=2;h.view.room.tables[2].phase=room::TablePhase::Playing;
 h.view.room.tables[2].ready[0]=h.view.room.tables[2].ready[1]=true;
 std::string tableStatus;SetMenuStatusProbe([&](const char* status,Tone){tableStatus=status;});
 h.view.session.match=netplay::MatchState::PostMatch;h.view.canEditSelection=false;h.Frame();
 // The finished game's bookkeeping (result, receipt, drain) is the runtime's
 // job: the player sees one Ready for rematch control and presses it once.
 Check(row("ready").label=="Ready for rematch"&&row("ready").enabled,"Finished match hides Ready for rematch behind the result wait");
 Check(row("selection").detail.find("Unready")==std::string::npos,"Finished match incorrectly asks the player to Unready");
 Check(tableStatus.find("READY")==std::string::npos,"Post-match footer falsely reports READY");
 Check(tableStatus.find("Waiting for results")==std::string::npos,"Post-match footer exposes the result wait");
 const auto postMatchActions=h.actions.size();h.Choose("ready");
 Check(h.actions.size()==postMatchActions+1&&h.actions.back().command.kind==Kind::Rematch,"Pending result dropped the rematch press");
 h.view.readyRequested=true;h.Frame();
 Check(row("ready").label=="Readying up..."&&!row("ready").enabled,"In-flight Ready still offers a second press");
 Check(tableStatus.find("Readying up")!=std::string::npos,"In-flight Ready is not shown on the seat line");
 h.view.readyRequested=false;
 h.view.readyFailure="Your Ready did not go through.";h.view.readyFailureSequence=1;h.Frame();
 const auto beforeNotice=h.actions.size();h.Press(MenuInput::Select);
 Check(h.actions.size()==beforeNotice,"Dismissing the failure notice activated the focused row");
 h.Press(MenuInput::Select);
 Check(h.actions.size()==beforeNotice+1,"Ready unavailable after the failure notice was dismissed");
 h.view.room.tables[2].phase=room::TablePhase::Paused;h.Frame();
 Check(row("ready").label=="Result unresolved"&&!row("ready").enabled,"Unresolved result presented as Ready");
 SetMenuStatusProbe({});
 h.view.room.tables[2].phase=room::TablePhase::Waiting;
 h.view.room.tables[2].ready[0]=h.view.room.tables[2].ready[1]=false;
 h.view.session.match=netplay::MatchState::None;h.view.canEditSelection=true;
 SetMenuEntriesProbe({});h.view.room.tables[2].p2=2;h.view.canReady=true;
 h.Choose("ready");Check(h.actions.back().command.kind==Kind::Ready,"Ready journey");
 h.view.room.tables[2].ready[0]=true;h.Choose("ready");Check(h.actions.back().roomAction.kind==room::ActionKind::Unready,"Unready journey");
 // On the board, A on your own seat readies and unreadies, and one B leaves
 // the seat, ready or not. While the game is starting B takes Ready back
 // instead, and the legend says which.
 h.Screen("room");h.view.room.tables[2].ready[0]=false;h.Choose("table-2");
 Check(h.actions.back().command.kind==Kind::Ready,"A on your seat did not ready");
 h.view.room.tables[2].ready[0]=true;h.Press(MenuInput::Select);
 Check(h.actions.back().roomAction.kind==room::ActionKind::Unready,"A again did not unready");
 h.Press(MenuInput::Back);Check(h.actions.back().roomAction.kind==room::ActionKind::Unqueue&&h.actions.back().roomAction.table==2&&
  h.shell.Navigation().Screen()=="room","One B did not leave a readied seat");
 h.view.room.tables[2].ready[1]=true;h.view.room.tables[2].phase=room::TablePhase::Ready;h.view.room.tables[2].spectatorHold=true;
 h.Press(MenuInput::Back);Check(h.actions.back().roomAction.kind==room::ActionKind::Unready,"B while the start is held did not unready");
 h.view.room.tables[2].spectatorHold=false;raced=h.actions.size();h.Press(MenuInput::Back);
 Check(h.actions.size()==raced&&h.shell.Navigation().Screen()=="room","B during a starting game left the seat or the board");
 h.view.room.tables[2].phase=room::TablePhase::Waiting;h.view.room.tables[2].ready[0]=h.view.room.tables[2].ready[1]=false;
 // B away from your own card is the ordinary Back: Home, keeping the seat.
 h.Press(MenuInput::Right);raced=h.actions.size();h.Press(MenuInput::Back);
 Check(h.actions.size()==raced&&h.shell.Navigation().Screen()=="home","B off your own card gave up the seat instead of going Home");
 h.Press(MenuInput::Back);Check(h.shell.Navigation().Screen()=="room","B at Home did not return to the room");
 // Walking down past other tables to the toolbar still opens your own table.
 h.FocusOn("table-2");h.Choose("options");
 std::vector<MenuEntry> optionRows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){optionRows=rows;});h.Frame();SetMenuEntriesProbe({});
 Check(h.shell.Navigation().Screen()=="room-table"&&std::any_of(optionRows.begin(),optionRows.end(),[](const MenuEntry& e){return e.id=="ready";}),
  "Table options opened a table the player only passed");
 // A on another table's card opens its options while you keep your seat: its
 // rules, and a host's recovery for its unresolved result, by pad or mouse.
 h.Screen("room");h.view.room.tables[0].phase=room::TablePhase::Paused;raced=h.actions.size();
 h.Choose("table-0");SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){optionRows=rows;});h.Frame();SetMenuEntriesProbe({});
 const auto hasRow=[&](const char* id){return std::any_of(optionRows.begin(),optionRows.end(),[&](const MenuEntry& e){return e.id==id;});};
 Check(h.shell.Navigation().Screen()=="room-table"&&hasRow("room-rules")&&hasRow("cancel-result")&&!hasRow("ready"),
  "A on another table did not open its options");
 h.Choose("room-rules");Check(h.shell.Navigation().Screen()=="room-rules","Another table's rules were unreachable");
 h.Press(MenuInput::Back);h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="room"&&h.actions.size()==raced,"Looking at another table sent a room action");
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){centres[id]=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);});
 click("table-0");SetMenuCardProbe({});ImGui::GetIO().AddMousePosEvent(-1,-1);
 Check(h.shell.Navigation().Screen()=="room-table"&&h.actions.size()==raced,"Clicking another table did not open its options");
 h.view.room.tables[0].phase=room::TablePhase::Idle;
 // A watcher reaches another table's options through the chooser's last
 // option, keeping the watch.
 h.view.room.members[0].seat=-1;h.view.room.tables[2].p1=0;h.view.room.tables[2].spectators={1};
 h.view.room.tables[0].phase=room::TablePhase::Paused;h.Screen("room");raced=h.actions.size();
 h.Choose("table-0");Check(h.shell.Navigation().Choosing(),"A watcher's A on another table did not open its chooser");
 h.Press(MenuInput::Right);h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){optionRows=rows;});h.Frame();SetMenuEntriesProbe({});
 Check(h.shell.Navigation().Screen()=="room-table"&&hasRow("room-rules")&&hasRow("cancel-result")&&h.actions.size()==raced,
  "A watcher could not look at another table's options");
 h.Choose("room-rules");h.Press(MenuInput::Back);h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="room"&&h.actions.size()==raced&&h.view.room.tables[2].spectators.size()==1,"Looking at another table changed the watch");
 h.view.room.tables[0].phase=room::TablePhase::Idle;h.view.room.tables[2].spectators.clear();
 h.view.room.members[0].seat=0;h.view.room.tables[2].p1=1;
 h.Screen("room");h.FocusOn("table-2");
 // A queued member's B leaves the queue.
 h.view.room.members[0].seat=-1;h.view.room.tables[2].p1=3;h.view.room.tables[2].queue={1};h.Press(MenuInput::Back);
 Check(h.actions.back().roomAction.kind==room::ActionKind::Unqueue&&h.shell.Navigation().Screen()=="room","B did not leave the queue");
 h.view.room.members[0].seat=0;h.view.room.tables[2].p1=1;h.view.room.tables[2].queue.clear();
 h.Screen("room-table");
 h.view.room.tables[2].ready[0]=false;h.view.session.match=netplay::MatchState::PostMatch;
 h.Choose("ready");Check(h.actions.back().command.kind==Kind::Rematch,"Rematch journey");
 h.view.room.tables[2].phase=room::TablePhase::Paused;h.Choose("cancel-result");
 auto count=h.actions.size();++h.view.room.tables[2].matchGeneration;h.Frame();h.Press(MenuInput::Right);h.Press(MenuInput::Select);
 Check(h.actions.size()==count,"Stale confirmation cancelled a different game");h.Press(MenuInput::Back);
 h.Screen("room");h.Choose("leave");count=h.actions.size();h.Press(MenuInput::Select);Check(h.actions.size()==count,"Leave default was not Cancel");
 h.Choose("leave");h.Press(MenuInput::Right);h.Press(MenuInput::Select);Check(h.actions.size()==count+1&&h.actions.back().command.kind==Kind::LeaveRoom,"Confirmed leave");
  h.Screen("room");h.Choose("room-members");h.Choose("member-2");h.Choose("kick");
  h.view.room.members.pop_back();h.Frame();h.Press(MenuInput::Right);h.Press(MenuInput::Select);Check(h.actions.back().command.kind==Kind::LeaveRoom,"Removed member was kicked");
 h.Screen("room");h.Choose("room-chat");h.Choose("compose");ImGui::GetIO().AddInputCharactersUTF8("Hello");h.Frame();
 ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 count=h.actions.size();h.Press(MenuInput::Down);Check(h.actions.size()==count,"Text acceptance sent chat");
 h.Choose("send-chat");Check(h.actions.back().roomAction.kind==room::ActionKind::Chat&&h.actions.back().roomAction.text=="Hello","Explicit chat send");
  h.view.session.control=netplay::Health::Lost;h.Screen("room");count=h.actions.size();h.Choose("table-2");Check(h.actions.size()==count,"Lost connection submitted Ready");
 h.view={};h.view.controllerReady=h.view.canEditPreferences=h.view.canOpenRoom=true;h.Frame();h.Screen("interface");h.Choose("hud");h.Press(MenuInput::Left);
 count=h.actions.size();h.Frame(0,20);Check(h.actions.size()==count,"Autosave not coalesced");
 h.Frame(0,20);Check(h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.showMatchHud,"Autosave did not queue");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-size");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudSize==1,"HUD size did not save"); // Small by default; Right steps to Standard.
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-spacing");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudRaised,"HUD spacing did not save");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("scale");h.Press(MenuInput::Right);
 h.view.settingsError="Disk unavailable";h.Frame(0,45);h.Choose("retry-save");h.Frame();
 Check(h.actions.back().command.kind==Kind::SavePreferences,"Save retry missing");
 h.view.settingsError.clear();h.view.preferences=h.actions.back().preferences;h.Frame();
 h.view.discordPending=h.view.discordConfirm=h.view.discordCanSwitch=true;h.view.discordRevision=9;h.Frame();
 h.Choose("invite-switch");h.Press(MenuInput::Select);Check(h.actions.back().discordAction==discord::InviteAction::None,"Switch default not Cancel");
 h.Choose("invite-cancel");Check(h.actions.back().discordAction==discord::InviteAction::Cancel&&h.actions.back().discordRevision==9,"Discord cancellation");
 h.view.discordPending=false;h.Frame();h.Screen("home");h.Choose("profile");h.Choose("main-character");h.Press(MenuInput::Right);h.Press(MenuInput::Select);h.Frame(0,40);
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.mainFighter==1,"Profile main was not saved");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 h.Screen("profile");h.Choose("main-character");count=h.actions.size();
 h.Press(MenuInput::Right);
 ControllerNavigation profilePad;
 const auto physical=[&](unsigned mapped,unsigned raw){
  ControllerSample sample{3,0,true,ControllerButtons(mapped,3,raw)};
  profilePad.Update(sample,true,true,true);h.Frame(profilePad.Buttons());
 };
 physical(0,0);physical(0,0);physical(0x40,0x40000);physical(0,0);h.Frame(0,40);
 Check(h.actions.size()>count&&h.actions.back().preferences.mainFighter==2,"Physical A did not save profile portrait");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 Check(h.shell.Navigation().Screen()=="profile","Accepted portrait save did not return to Profile");
 h.Choose("main-character");count=h.actions.size();
 ImVec2 mouseTarget;
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){if(std::strcmp(id,"main-3")==0)mouseTarget=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);});
 h.Frame();SetMenuCardProbe({});
 auto& mouse=ImGui::GetIO();mouse.AddMousePosEvent(mouseTarget.x,mouseTarget.y);h.Frame();
 mouse.AddMouseButtonEvent(0,true);h.Frame();mouse.AddMouseButtonEvent(0,false);h.Frame(0,40);
 Check(h.actions.size()>count&&h.actions.back().preferences.mainFighter==3,"Mouse click did not save profile portrait");
 h.view.preferences=h.actions.back().preferences;h.Frame();Check(h.shell.Navigation().Screen()=="profile","Mouse portrait save did not confirm");
 // A rejected save must leave a labelled, actionable retry in the portrait grid.
 h.Choose("main-character");h.Press(MenuInput::Right);h.accept=false;h.Press(MenuInput::Select);h.Frame(0,40);
 Check(h.shell.Navigation().Screen()=="main-character","Failed portrait save closed the roster");
 bool retryLabel=false;
 SetMenuTextProbe([&](const char* id,float,float,float width,float available){
  if(std::strcmp(id,"retry-save")==0)retryLabel=width>0&&width<=available;
 });
 h.Frame();SetMenuTextProbe({});
 Check(retryLabel,"Portrait save retry has no visible label");
 h.accept=true;h.Choose("retry-save");h.Frame();
 Check(h.actions.back().command.kind==Kind::SavePreferences&&h.actions.back().preferences.mainFighter==4,"Portrait retry lost the selected main");
 h.view.preferences=h.actions.back().preferences;h.Frame();
 Check(h.shell.Navigation().Screen()=="profile","Retried portrait save did not return to Profile");
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
 Check(h.shell.Navigation().Focus()=="hud-size","Controller did not move to the row following the mouse-selected row");
 h.Screen("profile");h.Choose("main-character");h.Press(MenuInput::Right);h.Press(MenuInput::Select);h.Frame(0,40);
 h.view.preferences=h.actions.back().preferences;h.Frame(0,3);Check(status.find("Profile portrait saved:")==0,"Profile success notice missing");
 h.Frame(0,200);Check(status=="Saved","Success notice did not expire");SetMenuStatusProbe({});
 GameMenu recovery;recovery.navigation=RecoveryNavigation(true);sf4e::platform::ServiceSnapshot state;
 state.update.ok=state.update.updateAvailable=true;state.update.expectedSha256=std::string(64,'a');
 const auto frame=[&](unsigned held=0){SetMenuInput({held,0});ImGui::NewFrame();const auto choice=DrawRecoveryMenu(recovery,state,"",true);ImGui::Render();return choice;};
 frame();frame();frame(MenuInput::Down);frame();frame(MenuInput::Select);frame();
 Check(recovery.navigation.Confirming()&&!recovery.navigation.ConfirmSelected(),"Recovery install not defaulting to Cancel");
 Check(frame(MenuInput::Select)==RecoveryChoice::None,"Recovery default confirmation installed an update");frame();
 state.pending=true;state.downloadedBytes=100;state.totalBytes=200;frame();
 frame(MenuInput::Down);frame();Check(frame(MenuInput::Select)==RecoveryChoice::Cancel,"Recovery cancellation not reachable");
}
// The rules every GameMenu screen shares with the room: an open dialog owns
// the legend (Select names its highlighted button, Back cancels, shortcuts
// go), pad Select accepts a draft, information rows offer no Select, a reader
// scrolls and closes, a list choice opens on the saved option, and only a
// moving pointer takes the selection.
void DialogContract(){
 HeadlessImGui imgui;GameMenu menu;SetMenuGlyphs(3,0x40000,0x20000);
 menu.shortcutHints={{"X","Fighter"}};
 auto language=Value("language","Language","B","Pick one");language.choices={{"a","A"},{"b","B"},{"c","C"}};language.chosen="b";
 auto info=Row("record","Record","12 wins");info.info=true;
 auto reader=Row("licence","Licence","Long text");reader.reading=true;
 std::vector<MenuEntry> rows={ConfirmRow("leave","Leave room","Disconnect"),TextRow("name","Name","Kate",31),info,reader,language};
 std::set<std::string> legend;std::map<std::string,ImVec2> centres;std::map<std::string,std::pair<ImVec2,ImVec2>> boxes;
 SetMenuTextProbe([&](const char* id,float,float,float,float){if(!std::strncmp(id,"legend/",7))legend.insert(id+7);});
 SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){centres[id]=ImVec2((min.x+max.x)*.5f,(min.y+max.y)*.5f);boxes[id]={min,max};});
 auto frame=[&](unsigned held=0){legend.clear();imgui.io.DeltaTime=1.f/60;SetMenuInput({held,0});ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(imgui.io.DisplaySize);ImGui::Begin("Dialog test",nullptr,ImGuiWindowFlags_NoDecoration);
  const auto a=menu.Draw("TEST",rows);ImGui::End();ImGui::Render();return a;};
 auto press=[&](unsigned held){frame();const auto a=frame(held);frame();return a;};
 auto has=[&](const char* label){return legend.count(label)!=0;};
 frame();frame();
 Check(has("Review")&&has("Back")&&has("Fighter"),"Plain legend lost its Select, Back or shortcut");
 press(MenuInput::Select);frame();
 Check(menu.navigation.Confirming()&&has("Cancel")&&!has("Fighter")&&!has("Review"),"A confirmation did not take over the legend");
 press(MenuInput::Right);frame();Check(has("Leave room"),"The legend did not name the highlighted confirmation button");
 press(MenuInput::Back);Check(!menu.navigation.Confirming(),"Back did not cancel the confirmation");
 menu.navigation.Focus("name",rows);press(MenuInput::Select);frame();
 Check(menu.navigation.Editing()&&has("Accept")&&has("Cancel"),"The editor legend does not say Accept and Cancel");
 const auto accepted=press(MenuInput::Select);
 Check(accepted.kind==MenuAction::TextAccepted&&accepted.text=="Kate","Controller Select did not accept the draft");
 // A pointer moved onto Cancel keeps that highlight; the legend says so, and
 // the controller's Select then cancels rather than accepting.
 press(MenuInput::Select);imgui.io.AddMousePosEvent(1,1);frame();
 {const auto at=centres.at("edit/0");imgui.io.AddMousePosEvent(at.x,at.y);frame();frame();}
 Check(menu.navigation.Editing()&&!menu.navigation.EditAccepts()&&has("Cancel edit"),"The pointer's Cancel highlight was not kept or shown");
 Check(press(MenuInput::Select).kind==MenuAction::None&&!menu.navigation.Editing(),"Select after the pointer chose Cancel accepted the draft");
 imgui.io.AddMousePosEvent(1,1);frame();
 bool hidden=false;menu.ShowNotice("Advice","Heading","Don't show again",[&]{hidden=true;});frame();frame();
 Check(has("OK")&&has("Close")&&!has("Fighter"),"A notice kept the screen's legend");
 press(MenuInput::Right);frame();Check(has("Don't show again"),"The notice legend did not follow its highlighted button");
 press(MenuInput::Select);Check(hidden&&!menu.NoticeOpen(),"The notice's alternative was not taken");
 menu.navigation.Focus("record",rows);frame();frame();
 Check(!has("Review")&&!has("Select")&&press(MenuInput::Select).kind==MenuAction::None,"An information row offers Select");
 menu.navigation.Focus("licence",rows);frame();frame();Check(has("Read"),"A reader row does not say Read");
 press(MenuInput::Select);frame();Check(menu.navigation.Reading()&&has("Close"),"Select did not open the reader");
 frame(MenuInput::Down);frame(MenuInput::Down);
 Check(press(MenuInput::Back).kind==MenuAction::None&&!menu.navigation.Reading()&&menu.navigation.Screen()=="home","Back did more than close the reader");
 menu.navigation.Focus("language",rows);frame();frame();Check(has("Choose"),"A choice row does not say Choose");
 press(MenuInput::Select);Check(menu.navigation.Choosing()&&menu.navigation.ChoiceIndex()==1,"The choice did not open on the saved option");
 press(MenuInput::Down);const auto chosen=press(MenuInput::Select);
 Check(chosen.kind==MenuAction::Chosen&&chosen.id=="language"&&chosen.text=="c","Down and Select did not choose the next option");
 // A click is one intent: the value's middle opens the list without changing
 // it, its arrow adjusts without opening, and the list's own Cancel closes it.
 const auto click=[&](ImVec2 at){imgui.io.AddMousePosEvent(at.x,at.y);frame();imgui.io.AddMouseButtonEvent(0,true);const auto a=frame();
  imgui.io.AddMouseButtonEvent(0,false);const auto b=frame();frame();return a.kind!=MenuAction::None?a:b;};
 const auto row=boxes.at("language");const float y=(row.first.y+row.second.y)*.5f;
 auto a=click(ImVec2(row.second.x-110,y));
 Check(a.kind!=MenuAction::Adjust&&menu.navigation.Choosing(),"Clicking the language value changed it or did not open the list");
 a=click(centres.at("choice-cancel/0"));
 Check(a.kind==MenuAction::None&&!menu.navigation.Choosing(),"The list's Cancel chose something or left it open");
 a=click(ImVec2(row.second.x-12,y));
 Check(a.kind==MenuAction::Adjust&&a.delta==1&&!menu.navigation.Choosing(),"The language arrow did not only step the value");
 // Narrow, the value stacks under the label: a click on the label opens the
 // list, and only the value's own line holds the arrows.
 imgui.io.DisplaySize=ImVec2(400,900);frame();frame();
 const auto narrow=boxes.at("language");
 a=click(ImVec2(narrow.first.x+10,narrow.first.y+10));
 Check(a.kind!=MenuAction::Adjust&&menu.navigation.Choosing(),"A click on a stacked row's label adjusted it");
 press(MenuInput::Back);
 a=click(ImVec2(narrow.first.x+12,narrow.second.y-12));
 Check(a.kind==MenuAction::Adjust&&a.delta==-1&&!menu.navigation.Choosing(),"A stacked row's left arrow did not step down");
 imgui.io.DisplaySize=ImVec2(1280,960);frame();frame();
 // The pointer: resting it on a row changes nothing; moving it there selects.
 menu.navigation.Focus("leave",rows);imgui.io.AddMousePosEvent(1,1);frame();frame();
 const auto target=centres.at("record");imgui.io.AddMousePosEvent(target.x,target.y);frame();
 Check(menu.navigation.Focus()=="record","A moving pointer did not select the row under it");
 press(MenuInput::Up);frame();frame();
 Check(menu.navigation.Focus()=="name","A resting pointer took the selection back from the pad");
 // Readers keep their own scroll: scroll one long document, close it, and
 // the next opens at its heading; so does the first, read again.
 {std::string text;for(int i=0;i<200;++i)text+="Line "+std::to_string(i)+"\n";
  auto first=Row("first","First",text);first.reading=true;auto second=Row("second","Second",text);second.reading=true;
  rows={first,second};
  const auto top=[&]{const auto reader=boxes.at("reader"),heading=boxes.at("reader-heading");return heading.first.y>=reader.first.y-.5f;};
  menu.navigation.Focus("first",rows);press(MenuInput::Select);frame();
  Check(menu.navigation.Reading()&&top(),"A reader did not open at its heading");
  for(int i=0;i<60;++i)frame(MenuInput::Down);frame();
  Check(!top(),"Holding Down did not scroll the reader");
  press(MenuInput::Back);menu.navigation.Focus("second",rows);press(MenuInput::Select);frame();
  Check(menu.navigation.ReadingId()=="second"&&top(),"A second reader inherited the first one's scroll");
  press(MenuInput::Back);menu.navigation.Focus("first",rows);press(MenuInput::Select);frame();
  Check(top(),"A reread document did not open at its heading");
  press(MenuInput::Back);}
 // A long list opens on the saved option in view, however it was scrolled
 // when it last closed, by Back or by the mouse's Cancel.
 {auto many=Row("many","Many","Pick one");
  for(int i=0;i<30;++i)many.choices.push_back({"o"+std::to_string(i),"Option "+std::to_string(i)});
  many.chosen="o1";rows={many};menu.navigation.Focus("many",rows);frame();frame();
  const auto visible=[&]{const auto list=boxes.at("choice-list");const auto option=boxes.at("choice/o1");
   return option.first.y>=list.first.y-.5f&&option.second.y<=list.second.y+.5f;};
  for(const bool mouse:{false,true}){
   press(MenuInput::Select);frame();Check(menu.navigation.Choosing()&&visible(),"A long list did not open on its saved option");
   // The pointer comes to rest where the list opens, so the wheel scrolls it
   // without the pointer moving the highlight off the saved option.
   const auto list=boxes.at("choice-list");press(MenuInput::Back);
   imgui.io.AddMousePosEvent((list.first.x+list.second.x)*.5f,(list.first.y+list.second.y)*.5f);frame();frame();
   press(MenuInput::Select);frame();Check(menu.navigation.ChoiceIndex()==1,"A resting pointer moved the list's highlight");
   for(int i=0;i<6;++i){imgui.io.AddMouseWheelEvent(0,-5);frame();}
   Check(!visible(),"The wheel did not scroll the saved option away");
   if(mouse){const auto at=centres.at("choice-cancel/0");imgui.io.AddMousePosEvent(at.x,at.y);frame();
    imgui.io.AddMouseButtonEvent(0,true);frame();imgui.io.AddMouseButtonEvent(0,false);frame();}
   else press(MenuInput::Back);
   Check(!menu.navigation.Choosing(),"The long list did not close");
  }
  press(MenuInput::Select);frame();
  Check(menu.navigation.Choosing()&&menu.navigation.ChoiceIndex()==1&&visible(),"A reopened list selected its saved option out of view");
  press(MenuInput::Back);}
 SetMenuTextProbe({});SetMenuCardProbe({});
}
// Every screen a Back button can name has a name of its own, not its id.
void ScreenNames(){
 for(const char* screen:{"home","online","create","join","profile","main-character","selection","settings","player","defaults","interface",
   "discord","discord-invitation","assignment","about","room","room-table","room-rules","room-members","room-member","room-chat","room-admin",
   "roster","appearance","costumes","colors","ultra","stage","options","recording","history","recovery","updates"})
  if(!MenuScreenName(screen))throw std::runtime_error(std::string("Screen without a display name: ")+screen);
}
void ProfileRecords(){
 using namespace sf4e;netplay::ProfileRecord record;netplay::RandomRoomId roomA{};netplay::RandomRoomId roomB{};roomA[0]=1;roomB[0]=2;
 Check(record.Record(roomA,0,1,0,room::MatchResult::P1Win)&&record.wins==1,"Confirmed win not counted");
 Check(!record.Record(roomA,0,1,0,room::MatchResult::P1Win),"Duplicate result counted");
 Check(record.Record(roomB,0,1,0,room::MatchResult::P2Win)&&record.losses==1,"Distinct rooms with the same table/generation collided");
 for(auto result:{room::MatchResult::Draw,room::MatchResult::Abort,room::MatchResult::Cancel})Check(!record.Record(roomA,0,3,0,result),"Non-decision counted");
 Check(!record.Record(roomA,0,3,2,room::MatchResult::P1Win),"Spectator game counted");
 netplay::ProfileRecord restored;Check(netplay::ReadProfileRecord(nlohmann::json::parse(netplay::ProfileRecordJson(record).dump()),restored)&&restored.wins==1&&restored.losses==1,"Record did not round trip");
 Check(!restored.Record(roomA,0,1,0,room::MatchResult::P1Win),"Reload lost deduplication");
 Check(netplay::ReadProfileRecord({{"wins",std::uint64_t(2)},{"losses",std::uint64_t(1)},{"recent",{"1:0:1",netplay::ProfileResultKeyV2(roomB,0,1)}}},restored),"Legacy recent key was not readable");
 Check(!netplay::ReadProfileRecord({{"wins",std::uint64_t(0)},{"losses",std::uint64_t(0)},{"recent",{std::string(65,'x')}}},restored)&&!restored.available,"Recent key bound was not enforced");
 Check(!netplay::ReadProfileRecord({{"wins",-1},{"losses",0},{"recent",nlohmann::json::array()}},restored)&&!restored.available,"Invalid record accepted");
 netplay::ProfileRecord atLimit;atLimit.wins=netplay::ProfileRecord::MaximumGames/2;atLimit.losses=netplay::ProfileRecord::MaximumGames/2-1;
 Check(atLimit.Record(roomA,0,9,0,room::MatchResult::P1Win)&&atLimit.wins+atLimit.losses==netplay::ProfileRecord::MaximumGames,
  "Cap-minus-one record did not reach the exact total limit");
  Check(netplay::ReadProfileRecord(netplay::ProfileRecordJson(atLimit),restored)&&
  restored.wins==netplay::ProfileRecord::MaximumGames/2+1&&restored.losses==netplay::ProfileRecord::MaximumGames/2-1,
  "Exact persisted game limit was not readable");
 Check(!atLimit.Record(roomA,0,10,0,room::MatchResult::P1Win)&&atLimit.wins+atLimit.losses==netplay::ProfileRecord::MaximumGames,
  "Record after the exact total limit was accepted");
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
 selector.Navigation().Return();selector.Navigation().Push("colors");frame();frame();
 press(MenuInput::Right);Check(pick.color==0,"Gallery focus committed color");
 Check(press(MenuInput::Select)&&pick.color==2,"Color gallery ignored native availability gaps");
 press(MenuInput::Left,false);press(MenuInput::Select,false);Check(pick.color==2,"Locked gallery saved a choice");
 available.colors[1]=0;selector.Navigation().Return();selector.Navigation().Push("costumes");frame(0,false);
 // Missing palette data must not dereference an empty preview list.
 frame(0,false);
 available.colors[1]=5;selector.Navigation().Return();selector.Navigation().Push("ultra");frame();frame();
 std::vector<MenuEntry> ultras;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& rows){ultras=rows;});
 press(MenuInput::Down);Check(pick.ultra==0,"Ultra focus committed a choice");
 Check(press(MenuInput::Select)&&pick.ultra==1,"Ultra II did not save");
 press(MenuInput::Up);frame();
 Check(pick.ultra==1&&ultras.size()>=2&&ultras[1].value=="SAVED"&&ultras[0].value.empty(),"Saved Ultra has no persistent selection marker separate from focus");
 Dimps::GameEvents::VsMode::ConfirmedCharaConditions native{};
 selection::ToNative(pick,native);pick=selection::FromNative(native);frame();
 Check(pick.ultra==1&&ultras[1].value=="SAVED","Native selection round trip lost Ultra II");
 press(MenuInput::Select,false);Check(pick.ultra==1,"Locked Ultra selection changed");
 press(MenuInput::Down);press(MenuInput::Down);press(MenuInput::Select);frame();
 Check(pick.ultra==2&&ultras[2].value=="SAVED","Ultra Double did not save");
 selector.Navigation().Return();selector.Navigation().Push("ultra");frame();frame();
 Check(pick.ultra==2&&ultras[2].value=="SAVED","Reopening Ultra lost saved selection");
 SetMenuEntriesProbe({});
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
}
}
int main(){try{NativeReader();NavigationModel();NativeCapture();Journeys();TrainingJourneys();PresentationJourneys();DialogContract();ScreenNames();ProfileRecords();AppearanceGalleries();std::cout<<"Controller menu model, native reader/capture, profile record, and renderer journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
