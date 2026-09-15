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
#include <imgui.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
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
    Check(ControllerButtons(0x10,3,0x80000)==0,"Physical X must not select when bound to LP");
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
 nav.Update({0,time+1},rows);nav.Focus("0",rows);
 nav.Update({MenuInput::Down,time+1.1},rows);Check(nav.Focus()=="1","Immediate direction press");
 nav.Update({MenuInput::Down,time+1.2},rows);Check(nav.Focus()=="1","Repeat too early");
 nav.Update({MenuInput::Down,time+1.5},rows);Check(nav.Focus()=="2","Direction did not repeat");
 nav.NeutralGate();nav.Update({MenuInput::Select,time+2},rows);Check(nav.Update({0,time+3},rows).kind==MenuAction::None,"Neutral gate");
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
 Harness(){
  ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(1280,960);
  ApplyTheme(1);io.Fonts->Build();view.canEditPreferences=view.canOpenRoom=view.helperReady=view.controllerReady=view.canChangeController=true;
 }
 ~Harness(){ImGui::DestroyContext();}
 void Frame(unsigned buttons=0,int count=1){
  for(int i=0;i<count;++i){auto& io=ImGui::GetIO();io.DeltaTime=1.f/60;SetMenuInput({buttons,0});ImGui::NewFrame();
   shell.Draw(view,&open,[&](ShellAction a){actions.push_back(a);return accept;},[]{});
   ImGui::Render();}
 }
 void Press(unsigned b){Frame();Frame(b);Frame();}
 void Choose(const char* id){
  Frame();for(int i=0;i<100&&shell.Navigation().Focus()!=id;++i)Press(MenuInput::Up);
  for(int i=0;i<100&&shell.Navigation().Focus()!=id;++i)Press(MenuInput::Down);
  if(shell.Navigation().Focus()!=id)throw std::runtime_error(std::string("Journey item not reachable: ")+id+" on "+shell.Navigation().Screen()+" focused "+shell.Navigation().Focus());Press(MenuInput::Select);
 }
 void Screen(const char* id){shell.Navigation().Home();if(std::string(id)!="home")shell.Navigation().Push(id);Frame();}
};
void Journeys() {
 using namespace sf4e;
 Harness h;h.Frame();h.Screen("player");h.view.inputCapture=input::Capture::ReleaseAll;h.Frame();h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen()=="assignment"&&h.actions.back().inputAction==input::Action::Cancel,"Assignment Back did not only cancel capture");
 h.view.inputCapture=input::Capture::Idle;h.Frame();Check(h.shell.Navigation().Screen()=="player","Assignment lost return destination");
 h.Screen("home");h.Choose("online");Check(h.shell.Navigation().Screen()=="online","Online route");
 h.Choose("create");h.Choose("host");Check(h.actions.back().command.kind==Kind::HostRoom,"Create journey");
 h.Screen("join");h.Choose("invite-text");ImGui::GetIO().AddInputCharactersUTF8("sf4://invitation");h.Frame();
 ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);h.Frame();ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,false);h.Frame();
 h.Choose("join-now");Check(h.actions.back().command.kind==Kind::JoinInvite&&h.actions.back().command.invitation=="sf4://invitation","Join draft journey");
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
 h.Choose("table-2");h.Choose("queue");Check(h.actions.back().roomAction.kind==room::ActionKind::Queue&&h.actions.back().roomAction.table==2,"Table queue journey");
 h.Choose("watch");Check(h.actions.back().roomAction.kind==room::ActionKind::Watch,"Watch journey");
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
 std::string tableStatus;SetMenuStatusProbe([&](const char* status){tableStatus=status;});
 h.view.session.match=netplay::MatchState::PostMatch;h.view.canEditSelection=false;h.Frame();
 Check(row("ready").label=="Waiting for results","Finished match still offers Unready instead of waiting for results");
 Check(row("selection").detail.find("Unready")==std::string::npos,"Finished match incorrectly asks the player to Unready");
 Check(tableStatus.find("READY")==std::string::npos,"Post-match footer falsely reports READY");
 const auto postMatchActions=h.actions.size();h.Choose("ready");
 Check(h.actions.size()==postMatchActions,"Pending result submitted Ready");
 h.view.room.tables[2].phase=room::TablePhase::Paused;h.Frame();
 Check(row("ready").label=="Result unresolved"&&!row("ready").enabled,"Unresolved result presented as Ready");
 SetMenuStatusProbe({});
 h.view.room.tables[2].phase=room::TablePhase::Waiting;
 h.view.room.tables[2].ready[0]=h.view.room.tables[2].ready[1]=false;
 h.view.session.match=netplay::MatchState::None;h.view.canEditSelection=true;
 SetMenuEntriesProbe({});h.view.room.tables[2].p2=2;h.view.canReady=true;
 h.Choose("ready");Check(h.actions.back().command.kind==Kind::Ready,"Ready journey");
 h.view.room.tables[2].ready[0]=true;h.Choose("ready");Check(h.actions.back().roomAction.kind==room::ActionKind::Unready,"Unready journey");
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
  h.view.session.control=netplay::Health::Lost;h.Screen("room");h.Choose("table-2");count=h.actions.size();h.Choose("ready");Check(h.actions.size()==count,"Lost connection submitted Ready");
 h.view={};h.view.controllerReady=h.view.canEditPreferences=h.view.canOpenRoom=true;h.Frame();h.Screen("interface");h.Choose("hud");h.Press(MenuInput::Left);
 count=h.actions.size();h.Frame(0,20);Check(h.actions.size()==count,"Autosave not coalesced");
 h.Frame(0,20);Check(h.actions.back().command.kind==Kind::SavePreferences&&!h.actions.back().preferences.showMatchHud,"Autosave did not queue");
 h.view.preferences=h.actions.back().preferences;h.Frame();h.Choose("hud-size");h.Press(MenuInput::Right);h.Frame(0,45);
 Check(h.actions.back().preferences.matchHudSize==2,"HUD size did not save");
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
 Check(MenuScreenLabel("room-members")=="Members"&&MenuScreenLabel("player")=="Player & Controller","Internal screen keys leaked into Back labels");
 auto text=TextRow("name","Name","Player",31);auto value=Value("delay","Delay","2","Frames");
 auto confirm=ConfirmRow("leave","Leave room","Disconnect");
 Check(std::strcmp(MenuPrimaryHint(&text),"Edit")==0&&MenuPrimaryHint(&value)==nullptr&&std::strcmp(MenuPrimaryHint(&confirm),"Review")==0,"Contextual legend does not match action");
 text.enabled=false;Check(MenuPrimaryHint(&text)==nullptr,"Disabled field advertises submission");
 Harness h;std::string status;SetMenuStatusProbe([&](const char* text){status=text;});h.Screen("interface");ImVec2 leftArrow,otherRow;
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
 GameMenu recovery;recovery.navigation=MenuNavigation("close");sf4e::platform::ServiceSnapshot state;
 state.update.ok=state.update.updateAvailable=true;state.update.expectedSha256=std::string(64,'a');
 const auto frame=[&](unsigned held=0){SetMenuInput({held,0});ImGui::NewFrame();const auto choice=DrawRecoveryMenu(recovery,state,"",true);ImGui::Render();return choice;};
 frame();frame();frame(MenuInput::Down);frame();frame(MenuInput::Select);frame();
 Check(recovery.navigation.Confirming()&&!recovery.navigation.ConfirmSelected(),"Recovery install not defaulting to Cancel");
 Check(frame(MenuInput::Select)==RecoveryChoice::None,"Recovery default confirmation installed an update");frame();
 state.pending=true;state.downloadedBytes=100;state.totalBytes=200;frame();
 frame(MenuInput::Down);frame();Check(frame(MenuInput::Select)==RecoveryChoice::Cancel,"Recovery cancellation not reachable");
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
 ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(1280,960);ApplyTheme(1);io.Fonts->Build();
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
 Check(TrainingNavigation().Confirming()&&commands.empty()&&!TakeMenuReturn(),"Outside click dismissed or submitted training confirmation");
 Check(io.WantCaptureKeyboard&&io.WantCaptureMouse,"Flyout lost input capture outside its bounds");
 press(MenuInput::Right);press(MenuInput::Select);Check(commands.size()==1&&!TakeMenuReturn(),"Record returned before command acceptance");
 v.commandId=commands.back().requestId;v.commandAccepted=false;v.commandError="Fight not ready";frame();Check(!TakeMenuReturn(),"Failed command closed training");
 choose("play");Check(commands.back().action==training::Action::Play&&!TakeMenuReturn(),"Play command dispatch");
 v.commandId=commands.back().requestId;v.commandAccepted=true;frame();Check(TakeMenuReturn(),"Accepted playback did not return to practice");
 press(MenuInput::Back);Check(TrainingNavigation().Screen()=="home"&&!TakeMenuReturn(),"Back skipped the training root");
 press(MenuInput::Back);Check(TakeMenuReturn(),"Root Back did not return to practice");
 ImGui::DestroyContext();
}
}
int main(){try{NativeReader();NavigationModel();NativeCapture();Journeys();TrainingJourneys();PresentationJourneys();ProfileRecords();AppearanceGalleries();std::cout<<"Controller menu model, native reader/capture, profile record, and renderer journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
