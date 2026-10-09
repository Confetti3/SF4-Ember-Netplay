#include "../ui/ApplicationShell.hxx"
#include "../ui/ControllerNavigation.hxx"
#include "../ui/Theme.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/MenuGlyphs.hxx"
#include "../ui/MenuPresentation.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../common/MenuInputCapture.hxx"
#include "../netplay/ProfileRecordJson.hxx"
#include "../session/sf4e__SessionProtocol.hxx"
#include "imgui_test_support.hxx"
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <cstdint>
#include <cstring>
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
 // The pad's buttons reach the menu unchanged, so each one means what the menu expects of it.
 using namespace sf4e::input;
 Check(ControllerButtons(0,PadXInput,xinput::A)==MenuInput::Select&&ControllerButtons(0,PadXInput,xinput::B)==MenuInput::Back&&
  ControllerButtons(0,PadXInput,xinput::X)==MenuInput::Fighter&&ControllerButtons(0,PadXInput,xinput::Y)==MenuInput::Options&&
  ControllerButtons(0,PadXInput,xinput::View)==MenuInput::Chat,"An Xbox button reached the menu as another action");
 Check(ControllerButtons(0,PadXInput,xinput::Start)==Button::Menu&&(Button::Menu&(MenuInput::Fighter|MenuInput::Options|MenuInput::Chat))==0,"Start became a menu shortcut");
 Check(ControllerButtons(0x10,PadDirectInput,0)==MenuInput::Select&&ControllerButtons(0x10,PadKeyboard,0)==MenuInput::Select,"A non-Xbox pad lost its mapped select");
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
 // An option that cannot be picked is shown but never returned, and the
 // dialog stays open for another. A choice can also be asked for without a Select.
 rows[0].choices={{"sit","Sit","Updating.",false},{"watch","Watch"}};frame(0);nav.Cancel();
 Check(nav.Ask("seat",rows)&&nav.Choosing()&&nav.ChoiceIndex()==0,"A choice could not be asked for");
 a=press(MenuInput::Select);Check(a.kind==MenuAction::None&&nav.Choosing(),"A disabled option was picked");
 press(MenuInput::Right);a=press(MenuInput::Select);Check(a.kind==MenuAction::Chosen&&a.text=="watch","The enabled option beside a disabled one was not picked");
 rows[0].enabled=false;Check(!nav.Ask("seat",rows)&&!nav.Choosing(),"A disabled entry's choice was asked for");
 rows[0].enabled=true;nav.Cancel();
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
 Check(ControllerMenuAvailable(MenuContext::MainMenu)&&ControllerMenuAvailable(MenuContext::Spectating)&&
  !ControllerMenuAvailable(MenuContext::OfflineTraining)&&!ControllerMenuAvailable(MenuContext::Unavailable),
  "Controller navigation escaped the menu/spectator contexts");
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
// What Select opens is decided in one place, so the legend and navigation
// agree for a row that carries more than one behaviour.
void SelectPrecedence(){
 auto readable=Row("both","Both","text");readable.text=true;readable.reading=true;
 auto asked=Row("asked","Asked","x");asked.confirm=true;asked.choices={{"a","A"},{"b","B"}};
 std::vector<MenuEntry> rows={readable,asked};
 Check(!std::strcmp(MenuPrimaryHint(&rows[0]),sf4e::loc::T("menu.read")),"A text row that reads has the wrong legend");
 Check(!std::strcmp(MenuPrimaryHint(&rows[1]),sf4e::loc::T("menu.choose")),"A confirmation with choices has the wrong legend");
 MenuNavigation nav;
 nav.Choose(rows);Check(nav.Reading()&&!nav.Confirming()&&!nav.Choosing(),"A text row that reads did not open the reader");
 nav.Cancel();nav.Focus("asked",rows);nav.Choose(rows);
 Check(nav.Choosing()&&!nav.Confirming()&&!nav.Reading(),"A confirmation with choices did not open as a choice");
}
// The training flyout has no choice dialog: a row that carries choices must do
// nothing, not draw a confirmation whose buttons are dead or pick an option.
void FlyoutIgnoresChoices(){
 HeadlessImGui imgui;GameMenu menu;
 auto pick=Row("pick","Pick","x");pick.choices={{"a","A"},{"b","B"}};
 std::vector<MenuEntry> rows={pick};
 int chosen=0;
 auto frame=[&](unsigned held=0){imgui.io.DeltaTime=1.f/60;SetMenuInput({held,0});ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(imgui.io.DisplaySize);ImGui::Begin("Flyout test",nullptr,ImGuiWindowFlags_NoDecoration);
  const auto a=menu.Draw("TEST",rows,"",{},1,{},{},1.f);ImGui::End();ImGui::Render();if(a.kind==MenuAction::Chosen)++chosen;};
 auto press=[&](unsigned held){frame();frame(held);frame();};
 frame();frame();
 press(MenuInput::Select);Check(!menu.navigation.Choosing()&&!menu.navigation.Confirming(),"A flyout row with choices opened a dialog");
 press(MenuInput::Select);Check(chosen==0,"A flyout row with choices picked an option");
}
void ScreenNames(){
 for(const char* screen:{"home","online","create","join","profile","main-character","selection","settings","player","defaults","interface","developer",
   "discord","discord-invitation","public-rooms","identity","identity-backup","linked-accounts","tournament-matches","discord-connect","assignment","about","room","room-table","room-members","room-member","room-chat","room-admin",
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
}
int main(){try{NativeReader();NavigationModel();NativeCapture();DialogContract();SelectPrecedence();FlyoutIgnoresChoices();ScreenNames();ProfileRecords();std::cout<<"Controller menu model, native reader/capture, and profile record journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
