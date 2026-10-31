#include "../ui/ApplicationShell.hxx"
#include "../ui/ControllerNavigation.hxx"
#include "../ui/Theme.hxx"
#include "../ui/MenuRows.hxx"
#include "../ui/MenuGlyphs.hxx"
#include "../ui/MenuPresentation.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include "../common/MenuInputCapture.hxx"
#include "../common/TrainingPad.hxx"
#include "../common/TrainingCallInput.hxx"
#include "../training/TrainingSession.hxx"
#include "../netplay/ProfileRecordJson.hxx"
#include "../netplay/PublishCadence.hxx"
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
    // Select and Back are physical buttons (Pad::MenuButtons), never a fight function.
    Check(ControllerButtons(0x10 | 0x40 | 0x2000) == 0 && ControllerButtons(0x1000) == Button::Menu, "A fight function became a menu action");
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
 // RT, the fourth button of a stick's bottom row too, is the accessibility button, unless it confirms or goes back.
 Check(ControllerButtons(0,PadXInput,xinput::RT)==MenuInput::Describe&&ControllerButtons(0,PadDirectInput,xinput::RT)==MenuInput::Describe,"RT is not the accessibility button");
 MenuButtons onRt; onRt.confirm=xinput::RT; onRt.back=xinput::B;
 Check(ControllerButtons(0,PadXInput,xinput::RT,onRt)==MenuInput::Select,"RT bound to confirm also described");
 // A stick confirms on its third button and goes back on its second, as the game's menus do.
 Check(ControllerButtons(0x40,PadDirectInput,1u<<18)==MenuInput::Select&&ControllerButtons(0x80,PadDirectInput,1u<<17)==MenuInput::Back,"A stick lost the game's menu buttons");
 Check(ControllerButtons(0x10,PadDirectInput,1u<<19)==MenuInput::PrevTab&&ControllerButtons(0x400,PadDirectInput,1u<<21)==MenuInput::NextTab,"A stick's LP and HP do not step the tabs");
 {MenuButtons threePunch;threePunch.lightPunch|=1u<<20;threePunch.heavyPunch|=1u<<20;
  Check(ControllerButtons(0x430,PadDirectInput,1u<<20,threePunch)==0&&ControllerButtons(0x10,PadXInput,0)==0,"3P on a stick, or an Xbox pad's LP function, stepped a tab");}
 // A stick whose Select is the button that gives LP and whose Back gives HP:
 // each press is Select or Back alone, and no tab glyph names those buttons.
 {MenuButtons rebound;rebound.confirm=rebound.lightPunch;rebound.back=rebound.heavyPunch;
  Check(ControllerButtons(0x10,PadDirectInput,rebound.lightPunch,rebound)==MenuInput::Select&&ControllerButtons(0x400,PadDirectInput,rebound.heavyPunch,rebound)==MenuInput::Back,
   "A stick's Select or Back on its LP or HP button also stepped a tab");
  Check(!MenuShortcutGlyph(MenuInput::PrevTab,PadDirectInput,rebound)&&!MenuShortcutGlyph(MenuInput::NextTab,PadDirectInput,rebound),"A stick named a tab button that selects or goes back");
  // A second button that also gives LP still steps, and is still named.
  rebound.lightPunch|=1u<<22;
  Check(ControllerButtons(0x10,PadDirectInput,1u<<22,rebound)==MenuInput::PrevTab&&!std::strcmp(MenuShortcutGlyph(MenuInput::PrevTab,PadDirectInput,rebound),"LP"),"A stick's other LP button lost the tab step");}
 {MenuButtons onLb;onLb.confirm=xinput::LB;
  Check(ControllerButtons(0,PadXInput,xinput::LB,onLb)==MenuInput::Select&&!MenuShortcutGlyph(MenuInput::PrevTab,PadXInput,onLb)&&!std::strcmp(MenuShortcutGlyph(MenuInput::NextTab,PadXInput,onLb),"RB"),"LB bound to Select still stepped or named a tab");
  Check(!std::strcmp(MenuShortcutGlyph(MenuInput::PrevTab,PadDirectInput,{}),"LP")&&!std::strcmp(MenuShortcutGlyph(MenuInput::NextTab,PadDirectInput,{}),"HP")&&!MenuShortcutGlyph(MenuInput::Fighter,PadDirectInput,{}),"A stick names buttons it does not have");
  Check(!std::strcmp(MenuShortcutGlyph(MenuInput::PrevTab,PadKeyboard,{}),"PgUp")&&!MenuShortcutGlyph(MenuInput::Leave,PadXInput,{}),"The keyboard's tab key or the pad's Leave is wrong");}
 // Bound to X and Y, those confirm and go back; X and Y are then no shortcuts, and A and B do nothing.
 MenuButtons moved; moved.confirm=xinput::X; moved.back=xinput::Y;
 Check(ControllerButtons(0,PadXInput,xinput::X,moved)==MenuInput::Select&&ControllerButtons(0,PadXInput,xinput::Y,moved)==MenuInput::Back,"A bound button did not select or go back");
 Check(ControllerButtons(0,PadXInput,xinput::A|xinput::B,moved)==0&&ControllerButtons(0,PadXInput,xinput::View,moved)==MenuInput::Chat,"Moving the menu buttons broke the others");

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
 nav.Rearm();nav.Update({MenuInput::Select,time+2},rows);Check(nav.Update({0,time+3},rows).kind==MenuAction::None,"Neutral gate");
 // A direction held when a screen opens waits for its release; a press beside
 // it counts at once, and the direction counts again once released.
 nav.Focus("0",rows);nav.Rearm();nav.Update({MenuInput::Down,time+4},rows);nav.Update({MenuInput::Down,time+5},rows);
 Check(nav.Focus()=="0","A direction held at the gate moved focus");
 Check(nav.Update({MenuInput::Down|MenuInput::Select,time+5.1},rows).kind==MenuAction::Activate,"Select beside a held direction was swallowed");
 nav.Focus("0",rows);nav.Update({0,time+6},rows);nav.Update({MenuInput::Down,time+6.1},rows);Check(nav.Focus()!="0","A released direction did not count again");
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
 // The pad drives the main menu, the training controls and a menu opened over an export, nothing else.
 Check(ControllerMenuAvailable(MenuContext::MainMenu)&&ControllerMenuAvailable(MenuContext::OfflineTraining)&&
  ControllerMenuAvailable(MenuContext::ReplayExport)&&!ControllerMenuAvailable(MenuContext::Unavailable),"Controller navigation escaped its contexts");
 // During an export the pad drives a menu F10 opened, but Start stays the game's pause.
 Check(!sf4e::input::ControllerOpensMenu(MenuContext::ReplayExport)&&
  sf4e::input::ControllerOpensMenu(MenuContext::MainMenu)&&!sf4e::input::ControllerOpensMenu(MenuContext::OfflineTraining),"Start opened Ember over an export");
 // Over a Practice match this PC fights, Start opens Ember and the pad drives it;
 // the fight keeps the menu away from every other context.
 // So does a game this PC only watches, where Stop watching leaves it.
 for(const auto context:{MenuContext::PracticeMatch,MenuContext::WatchedMatch})
  Check(ControllerMenuAvailable(context)&&sf4e::input::ControllerOpensMenu(context)&&sf4e::input::MenuShownIn(context)&&
   sf4e::input::MenuOverFight(context),"Start could not open Ember over a fight it is shown over");
 // The WIN screen's post-match prompt is shown over the fight and the pad drives
 // it, but Start does not open the menu there.
 Check(ControllerMenuAvailable(MenuContext::MatchResult)&&!sf4e::input::ControllerOpensMenu(MenuContext::MatchResult)&&
  sf4e::input::MenuShownIn(MenuContext::MatchResult)&&sf4e::input::MenuOverFight(MenuContext::MatchResult),"The post-match prompt could not show over WIN");
 Check(!sf4e::input::MenuShownIn(MenuContext::OfflineTraining),"Ember's menu was shown over offline Training's controls");
 for(const auto context:{MenuContext::Unavailable,MenuContext::MainMenu,MenuContext::OfflineTraining,MenuContext::ReplayExport,
  MenuContext::PracticeMatch,MenuContext::WatchedMatch,MenuContext::MatchResult}){
  Check(sf4e::input::FightKeepsMenuAway(true,context)==!sf4e::input::MenuOverFight(context)&&!sf4e::input::FightKeepsMenuAway(false,context),
   "A fight kept the menu away where it is shown over the fight, or let it in elsewhere");
 }
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
 // A direction held from the fight when the menu shows waits for its release,
 // but a confirm pressed beside it counts at once.
 adapter.Update(device,true,true,false);device.buttons=ControllerSample::Down;adapter.Update(device,true,true,true);
 Check(!adapter.Buttons(),"Held opening direction activated");
 device.buttons=ControllerSample::Down|ControllerSample::Confirm;adapter.Update(device,true,true,true);
 Check(adapter.Buttons()==ControllerSample::Confirm,"A press beside a held direction was swallowed");
 device.buttons=0;adapter.Update(device,true,true,true);device.buttons=ControllerSample::Down;adapter.Update(device,true,true,true);
 Check(adapter.Buttons()==ControllerSample::Down,"A released direction did not count again");
 device.buttons=0;adapter.Update(device,true,true,true);device.buttons=32;
 adapter.Update(device,true,true,true);adapter.Update(device,true,false,true);Check(adapter.MenuGuard(),"Main-menu suppression not drained");
 device.buttons=0;adapter.Update(device,true,false,true);Check(!adapter.MenuGuard(),"Main menu never released");
 // The overlay enables the adapter whatever the window's focus (only a device
 // being assigned stops it): behind another window Start opens the hidden
 // menu and the shown menu takes presses, with no refocus between.
 ControllerNavigation behind;ControllerSample pad{0,0,true,0};
 behind.Update(pad,true,false,true);behind.Update(pad,true,false,true);
 pad.buttons=ControllerSample::Menu;behind.Update(pad,true,false,true);
 Check(behind.OpenRequested(),"Start did not open the menu behind another window");
 pad.buttons=0;behind.Update(pad,true,true,true);
 pad.buttons=ControllerSample::Down;behind.Update(pad,true,true,true);
 Check(behind.Buttons()==ControllerSample::Down&&behind.FocusRequested(),"The shown menu ignored the pad behind another window");
}
// The menu reads the pad from the runtime's published snapshot, published as
// netplay::PublishCadence says. Over a Practice fight or a watched game the
// pad drives Ember's menu, so every tick is published: a Select or a Back
// pressed and released between two otherwise scheduled publications reaches
// the menu, once each, for no longer than it was held. A fight without menu navigation stays
// throttled, its fingerprint unchanged.
void PracticeFightPresses(){
 using sf4e::input::MenuContext;
 // The first two ticks find the pad and arm the menu. Each press lasts one
 // tick and none falls on a backstop tick (0, 6, 12).
 const std::uint32_t ticks[]={0,0,Button::Confirm,0,Button::Back,0,0,0,Button::Confirm,0,Button::Back,0,0};
 constexpr unsigned count=sizeof(ticks)/sizeof(ticks[0]);
 for(const auto context:{MenuContext::PracticeMatch,MenuContext::WatchedMatch,MenuContext::Unavailable}){
  const bool navigable=sf4e::input::ControllerMenuAvailable(context);
  sf4e::netplay::PublishCadence cadence;ControllerNavigation nav;
  ControllerSample published;published.deviceType=sf4e::input::PadXInput;published.deviceIndex=0;published.connected=true;
  unsigned publications=0,selects=0,selectTicks=0,backs=0;std::uint32_t previous=0;
  for(const auto buttons:ticks){
   if(cadence.Due(navigable,[]{return std::uint64_t{7};})){++publications;published.buttons=buttons;}
   nav.Update(published,navigable,true,true);
   const auto out=nav.Buttons();
   if(out&Button::Confirm){++selectTicks;if(!(previous&Button::Confirm))++selects;}
   if(nav.BackRequested())++backs;
   previous=out;
  }
  if(navigable)Check(publications==count&&selects==2&&selectTicks==2&&backs==2,
   "A press between scheduled publications was lost or held over a fight the menu is shown over");
  else Check(publications==3,"A fight without menu navigation was no longer throttled");
 }
}
// The pad in offline Training: Back tapped resets, held saves, and Start
// pressed while Back is held opens the controls or closes them. None of the
// three clashes with another: the chord never resets or saves, a hold saves
// once, and the game's pause (Start first) never opens or resets.
// R3 on an Xbox pad: a press opens or closes the controls on its own, once
// per press, never under the call, and the game never sees it.
void TrainingPadQuick(){
 using namespace sf4e::input;
 TrainingPadGesture pad;double now=0;
 const auto step=[&](unsigned physical,bool open=false,TrainingCall call=TrainingCall::None){now+=1.0/60;return pad.Update(physical,open,now,call);};
 const unsigned quick=PhysicalQuick;
 auto e=step(quick);Check(e.open&&!e.close&&!e.reset&&!e.save&&(e.owned&quick),"R3 did not open the controls");
 Check(!step(quick).Any(),"Holding R3 acted twice");
 step(0);e=step(quick,true);Check(e.close&&!e.open,"R3 did not close the open controls");
 step(0);Check(!step(quick,false,TrainingCall::Called).Any(),"R3 opened the controls under the call");
 step(0);step(PhysicalBack);Check(!step(PhysicalBack|quick).open,"R3 opened the controls while Back was held");
}
void TrainingPadChord(){
 using namespace sf4e::input;
 TrainingPadGesture pad;double now=0;
 const auto step=[&](unsigned physical,bool open=false,double seconds=1.0/60){now+=seconds;return pad.Update(physical,open,now);};
 const unsigned back=PhysicalBack,start=PhysicalStart,both=back|start;
 auto e=step(back);Check(e.down&&!e.reset&&!e.save&&!e.open&&!e.close,"Back did not mark where the fighters stand");
 Check(!step(back,false,.2).Any(),"A short hold did more than wait");
 e=step(0);Check(e.reset&&!e.save&&!e.down,"A tap of Back did not reset");
 // Tap or hold is the time Back was down, also when the release is the
 // first sample past half a second (a stalled poll): that is a save.
 step(back);Check(!step(back,false,.49).Any(),"Back did more than wait under half a second");
 e=step(0,false,.02);Check(e.save&&!e.reset,"A release past half a second reset instead of saving");
 step(back);e=step(0,false,.6);Check(e.save&&!e.reset,"A release that was the first sample past half a second reset");
 step(back);e=step(0,false,.49);Check(e.reset&&!e.save,"A release under half a second saved");
 // A chord's Back is neither, however long, and a save is made once.
 step(back);step(both);e=step(0,false,1);Check(!e.save&&!e.reset,"A chord's late release saved or reset");
 step(back);step(back,false,.6);Check(!step(0).Any(),"A hold saved again as it was let go");
 step(back);Check(!step(back,false,.45).save,"Back saved before half a second");
 e=step(back,false,.06);Check(e.save&&!e.reset,"Holding Back did not save");
 Check(!step(back,false,1).Any(),"Holding Back saved twice");
 Check(!step(0).Any(),"Letting go of a hold also reset");
 step(back);e=step(both);Check(e.open&&!e.close&&!e.reset&&!e.save,"Back then Start did not open the controls");
 Check(!step(both,true,1).Any(),"The chord also saved or acted twice");
 Check(!step(back,true).Any()&&!step(0,true).Any(),"Letting go after the chord reset the position");
 e=step(back,true);Check(!e.Any(),"Back alone acted while the controls were open");
 Check(!step(back,true,1).Any()&&!step(0,true).Any(),"Back held or tapped in the controls reset or saved");
 step(back,true);e=step(both,true);Check(e.close&&!e.open,"Back then Start did not close the controls");step(0);
 // A DirectInput pad's Back is also the menu's Back: the controls may close
 // before Start comes, and the chord must not open them again.
 step(back,true);e=step(both,false);Check(e.close&&!e.open,"The chord reopened controls its own Back had closed");step(0);
 // Start first is the game's pause: Back under it does nothing here.
 step(start);e=step(both);Check(!e.Any(),"Back pressed under Start acted");
 Check(!step(both,false,1).Any()&&!step(start).Any()&&!step(0).Any(),"Start then Back reset, saved or opened");
 // Back under the game's pause menu is the menu's, also when the pause ends
 // before Back is let go or held long enough to save.
 e=pad.Update(back,false,now+=1.0/60,TrainingCall::None,true);Check(!e.Any(),"Back under the pause acted");
 Check(!step(back,false,.1).Any()&&!step(0).Any(),"Back pressed under the pause reset after it");
 pad.Update(back,false,now+=1.0/60,TrainingCall::None,true);Check(!step(back,false,1).Any()&&!step(0).Any(),"Back held from under the pause saved");
 // After a reset of the gesture, a Back still held counts only once pressed again.
 step(back);pad.Reset();Check(!step(back,false,1).Any()&&!step(0).Any(),"A Back held through a reset acted");
 Check(step(back).down,"Back did not work again after a reset");step(0);
 // Called back by a room, with go now offered on this pad: a fresh Back is
 // go now and nothing else, held or not, and no chord follows it.
 const auto called=[&](unsigned physical,TrainingCall call,double seconds=1.0/60){now+=seconds;return pad.Update(physical,false,now,call);};
 e=called(back,TrainingCall::GoNow);Check(e.goNow&&!e.down&&!e.reset&&!e.save&&!e.open,"A fresh Back under the call did not go now alone");
 Check(!called(back,TrainingCall::GoNow,1).Any(),"Holding go now's Back saved or went now twice");
 Check(!called(both,TrainingCall::GoNow).Any(),"Start after go now's Back opened the controls");
 Check(!called(back,TrainingCall::GoNow).Any()&&!called(0,TrainingCall::GoNow).Any(),"Letting go of go now's Back reset the position");
 // The banner ends while go now's Back is still held: its release does nothing.
 called(back,TrainingCall::GoNow);
 Check(!called(back,TrainingCall::None,1).Any()&&!called(0,TrainingCall::None).Any(),"Go now's Back saved or reset after the banner went");
 // A Back held from before the call is not go now; it ends as the tap or
 // hold it began as.
 e=called(back,TrainingCall::None);Check(e.down,"Back did not begin a tap");
 e=called(back,TrainingCall::GoNow);Check(!e.goNow,"A Back held from before the call went now");
 e=called(0,TrainingCall::GoNow);Check(e.reset&&!e.goNow,"A tap begun before the call did not end as a tap");
 called(back,TrainingCall::None);e=called(back,TrainingCall::GoNow,.6);Check(e.save&&!e.goNow,"A hold begun before the call did not save");
 called(0,TrainingCall::GoNow);
 // Under the call the chord opens nothing, whether go now is offered on this
 // pad or not, and its Back neither resets nor saves.
 for(const TrainingCall call:{TrainingCall::Called,TrainingCall::GoNow}){
  called(back,TrainingCall::None);e=called(both,call);Check(!e.open&&!e.close&&!e.goNow,"Back then Start opened the controls under the call");
  Check(!called(both,call,1).Any()&&!called(0,call).Any(),"The chord under the call saved or reset");
 }
 // Called with go now not offered on this pad (a DirectInput pad, or under a
 // window): Back stays the position's.
 e=called(back,TrainingCall::Called);Check(e.down&&!e.goNow,"Back went now where go now is not offered");
 Check(called(0,TrainingCall::Called).reset,"Back under the call without go now did not reset");
 // Start first is still the game's pause: Back under it is not go now.
 called(start,TrainingCall::GoNow);e=called(both,TrainingCall::GoNow);Check(!e.Any(),"Back pressed under Start went now");
 called(0,TrainingCall::GoNow);
 // A chord's Start is the gesture's, down, held and let go, whether or not
 // it opens anything: under the call too, and after go now's Back, so the
 // game never takes it for its pause. The game's own pause (Start first)
 // and a Back already held when the gesture began are left to the game.
 const auto owned=[](const TrainingPadEvents& events){return (events.owned&PhysicalStart)!=0;};
 for(const TrainingCall call:{TrainingCall::None,TrainingCall::Called,TrainingCall::GoNow}){
  Check(!owned(called(back,call)),"Back alone took Start from the game");
  Check(owned(called(both,call))&&owned(called(both,call,1)),"The chord's Start reached the game");
  Check(owned(called(back,call)),"The chord's Start was not kept from the game as it was let go");
  Check(!owned(called(back,call))&&!owned(called(0,call)),"Start stayed the gesture's after it was let go");
  // Back let go before Start: Start stays the gesture's until it is let go.
  called(back,call);called(both,call);Check(owned(called(start,call)),"The chord's Start went to the game when Back was let go first");
  Check(owned(called(0,call))&&!owned(called(0,call)),"The chord's Start was not let go with it");
 }
 called(start,TrainingCall::None);Check(!owned(called(both,TrainingCall::None))&&!owned(called(start,TrainingCall::None)),"The game's pause Start was taken");
 called(0,TrainingCall::None);
 called(back,TrainingCall::None);pad.Reset();Check(!owned(called(both,TrainingCall::None)),"A Back held through a reset took Start");
 called(0,TrainingCall::None);
}
// What the gesture asks for crosses to the drawing thread in order, one event
// each, with the battle and the pad owner it was pressed under: two gestures
// between frames stay two, a full queue drops rather than merges, and a
// change of owner drops what waits and what is still to be posted.
void TrainingPadEventOrder(){
 using namespace sf4e::input;
 using Kind=TrainingPadEvent::Kind;
 TrainingControls queue;
 const auto event=[&](Kind kind,std::uint64_t generation,float x=0){TrainingPadEvent e;e.kind=kind;e.generation=generation;e.epoch=queue.Epoch();e.place[0]=x;return e;};
 Check(queue.Post(event(Kind::Save,4,120))&&queue.Post(event(Kind::Reset,4))&&queue.Post(event(Kind::Save,4,200)),"The queue refused a gesture");
 auto taken=queue.Take();
 Check(taken.size()==3&&taken[0].kind==Kind::Save&&taken[0].place[0]==120&&taken[1].kind==Kind::Reset&&taken[2].kind==Kind::Save&&taken[2].place[0]==200,
  "Gestures between frames were merged or reordered, or a save lost where its press was");
 Check(queue.Take().empty(),"An event was taken twice");
 for(std::size_t i=0;i<TrainingControls::MostEvents;++i)queue.Post(event(Kind::Reset,4));
 Check(!queue.Post(event(Kind::Save,4))&&queue.Take().size()==TrainingControls::MostEvents,"A full queue took one more or merged it");
 // An owner change between posting and taking drops the waiting event, and
 // one made under the old owner and posted after is dropped too.
 const auto stale=event(Kind::Save,4);
 queue.Post(event(Kind::Reset,4));queue.Invalidate();
 Check(queue.Take().empty(),"An event outlived its pad owner");
 Check(!queue.Post(stale)&&queue.Take().empty(),"An event made under the old owner was posted");
 Check(queue.Post(event(Kind::Reset,5))&&queue.Take().size()==1,"The new owner's event was dropped");
}
// Each Back press keeps the owner it went down under: the owner changing
// before the gesture ends (focus lost and back between two polls, another
// battle) drops it whole, the controls included. A batch taken before the
// owner changed is not applied under the new one.
void TrainingPadOwnership(){
 using namespace sf4e::input;
 TrainingPadInput pad;TrainingControls controls;double now=0;
 const float place[2]={100,300};
 PadOwner owner;owner.generation=4;owner.epoch=controls.Epoch();
 const auto step=[&](unsigned physical,const PadOwner& current,double seconds=1.0/60){now+=seconds;return pad.Update(physical,controls,now,TrainingCall::None,current,place);};
 const unsigned back=PhysicalBack,both=PhysicalBack|PhysicalStart;
 auto r=step(back,owner);
 Check(r.events.pressed&&r.owner==owner&&r.place[0]==100,"A press did not take its owner and place");
 controls.Invalidate();
 PadOwner regained=owner;regained.epoch=controls.Epoch();
 r=step(both,regained);
 Check(!r.events.open&&!controls.Open(),"A chord finished under a new pad owner opened the controls");
 Check(!step(0,regained).events.Any(),"A press from the old owner reset or saved");
 r=step(back,regained);step(both,regained);
 Check(controls.Open(),"A fresh chord under the new owner did not open the controls");
 step(0,regained);controls.Close();
 // Another battle while Back is held: no save comes of the old press.
 step(back,regained);PadOwner next=regained;next.generation=5;
 Check(!step(back,next,1).events.save&&!step(0,next).events.Any(),"A press from the last battle saved in the next");
 r=step(back,next);r=step(back,next,.6);
 Check(r.events.save&&r.owner==next,"A hold under one owner did not save for it");
 step(0,next);
 // The focus goes while a chord's update is under way: the poll read its
 // owner before, and the controller refuses the chord after. The controls
 // stay closed and nothing of the chord is published.
 step(back,next);
 controls.LoseFocus();
 r=step(both,next);
 Check(!r.events.open&&!controls.Open(),"A chord whose owner went during its update opened the controls");
 step(0,next);
 controls.GainFocus();
 // Open, then the focus goes and comes back with no frame drawn between:
 // the controls closed with the focus, so the next chord opens them rather
 // than closing what the old owner left open.
 PadOwner back1=next;back1.epoch=controls.Epoch();
 step(back,back1);step(both,back1);step(0,back1);
 Check(controls.Open(),"The chord did not open the controls");
 controls.LoseFocus();
 Check(!controls.Open(),"The controls stayed open when the focus went");
 controls.GainFocus();
 PadOwner back2=back1;back2.epoch=controls.Epoch();
 step(back,back2);step(both,back2);step(0,back2);
 Check(controls.Open(),"A chord after the focus came back closed controls the old owner left open");
 // The focus goes between a poll's focus read and its owner: the owner it
 // reads after the loss is of a period without the focus, so a token taken
 // then says so, and the Back recorded under it opens nothing once the focus
 // is back, with no frame drawn between: the next poll's token is of the new
 // period, and the press is dropped with its old owner.
 controls.Close();
 controls.LoseFocus();
 Check(!controls.Sample().focused,"A token taken without the focus said it had the focus");
 PadOwner stale=back2;stale.epoch=controls.Epoch();
 step(back,stale);
 controls.GainFocus();
 PadOwner fresh=stale;fresh.epoch=controls.Sample().epoch;
 r=step(both,fresh);step(0,fresh);
 Check(!r.events.open&&!controls.Open(),"A Back sampled as the focus went opened the controls once it came back");
 step(back,fresh);step(both,fresh);step(0,fresh);
 Check(controls.Open(),"A chord in the new focus period did not open the controls");
 // The focus goes between a frame's HUD result and its publication, and
 // comes back before another frame: the frame's open is refused, as F6's
 // and F7's are, since its token is of the period that ended.
 {
  TrainingControls hud;
  const auto token=hud.Sample();
  hud.LoseFocus();hud.GainFocus();
  Check(!hud.Open(token)&&!hud.Toggle(token)&&!hud.Open()&&!CapturesInput(false,hud),"A frame from before the focus went opened the controls after it came back");
  Check(hud.Open(hud.Sample())&&hud.Open(),"A frame in the new focus period did not open the controls");
  Check(hud.Toggle(token)&&!hud.Open(),"F6 under an old token did not close the controls");
  hud.LoseFocus();
  const auto unfocused=hud.Sample();
  Check(!unfocused.focused&&!hud.Open(unfocused)&&!hud.Open(),"A frame without the focus opened the controls");
 }
 // A batch taken just before the owner changed is not applied.
 TrainingControls queue;TrainingPadEvent event;event.kind=TrainingPadEvent::Kind::Save;event.generation=5;event.epoch=queue.Epoch();
 Check(queue.Post(event),"The queue refused an event of its owner");
 const auto taken=queue.Take();queue.Invalidate();
 Check(taken.size()==1&&!queue.Current(taken[0]),"An event taken before the owner changed was still current");
 // The game's input is taken while the controls are open, read from their
 // controller as it is now. A frame reads them closed, a chord opens them,
 // and the frame then publishes its menu's part as not shown: the input is
 // still taken, so the game's caches are cleared.
 TrainingControls race;PadOwner live;live.generation=1;live.epoch=race.Epoch();
 TrainingPadInput chord;double at=0;
 const bool frameSawOpen=race.Open();
 chord.Update(back,race,at+=.02,TrainingCall::None,live,place);chord.Update(both,race,at+=.02,TrainingCall::None,live,place);
 const bool shellShown=frameSawOpen&&false;
 MenuInputCapture gate;
 Check(!frameSawOpen&&CapturesInput(shellShown,race)&&gate.Update(CapturesInput(shellShown,race),0),"A frame's late publication released the input the chord took");
}
// The controls' open state has one owner. Two chords before a frame open and
// then close them, from either state; F6 and the controls' own Back act on
// the same state, and a chord whose Back already closed the controls (a
// DirectInput pad's Back is the menu's too) does not open them again.
void TrainingFlyoutOwner(){
 using namespace sf4e::input;
 const float place[2]={0,0};
 for(const bool start:{false,true}){
  TrainingPadInput pad;TrainingControls controls;if(start)controls.Open(controls.Sample());double now=0;
  PadOwner owner;owner.generation=1;owner.epoch=controls.Epoch();
  const auto step=[&](unsigned physical){now+=1.0/60;return pad.Update(physical,controls,now,TrainingCall::None,owner,place);};
  for(int chord=0;chord<2;++chord){step(PhysicalBack);step(PhysicalBack|PhysicalStart);step(0);}
  Check(controls.Open()==start,"Two chords before a frame did not open and close the controls");
  step(PhysicalBack);step(PhysicalBack|PhysicalStart);step(0);
  Check(controls.Open()==!start,"One chord did not change the controls");
  // F6 between chords: the next chord acts on what F6 left.
  controls.Toggle(controls.Sample());
  step(PhysicalBack);step(PhysicalBack|PhysicalStart);step(0);
  Check(controls.Open()==!start,"A chord after F6 did not act on what F6 left");
 }
 TrainingPadInput pad;TrainingControls controls;controls.Open(controls.Sample());double now=0;
 PadOwner owner;owner.generation=1;owner.epoch=controls.Epoch();
 const auto step=[&](unsigned physical){now+=1.0/60;return pad.Update(physical,controls,now,TrainingCall::None,owner,place);};
 step(PhysicalBack);controls.Close();// the controls' own Back closed them
 step(PhysicalBack|PhysicalStart);step(0);
 Check(!controls.Open(),"The chord reopened controls its own Back had closed");
}
// Go now is taken only for the call that stands and only while the press is
// nobody else's; the request names that call, so once the call has changed
// the call's owner sees it is not for the call that stands now.
void GoNowRequests(){
 using namespace sf4e::input;
 GoNowGate gate;
 CallIdentity call;call.roomEpoch=7;call.table=1;call.opponent=2;call.generation=40;
 Check(!gate.Press(GoNowGate::Source::Keyboard,true)&&gate.Take().empty(),"Go now was taken with no call");
 gate.Offer(call);
 Check(!gate.Press(GoNowGate::Source::Keyboard,false)&&gate.Take().empty(),"Go now was taken under the pause menu or Ember's menu");
 Check(gate.Press(GoNowGate::Source::Pad,true)&&gate.Press(GoNowGate::Source::Keyboard,true),"Go now was not taken for the call");
 auto taken=gate.Take();
 Check(taken.size()==2&&taken[0].call==call&&taken[0].source==GoNowGate::Source::Pad&&taken[1].source==GoNowGate::Source::Keyboard,"Go now's requests lost their call or their order");
 // Taken for one call, carried out after another took its place: not that call.
 gate.Press(GoNowGate::Source::Keyboard,true);
 CallIdentity next=call;next.opponent=3;gate.Offer(next);
 taken=gate.Take();
 Check(taken.size()==1&&taken[0].call!=next,"A go now taken for an earlier call matched the call after it");
 CallIdentity room=call,table=call,battle=call,nobody=call;room.roomEpoch=8;table.table=2;battle.generation=41;nobody.opponent=0;
 Check(room!=call&&table!=call&&battle!=call&&nobody!=call&&!nobody.Live(),"Two different calls compared the same");
 gate.Offer(CallIdentity{});
 Check(!gate.Press(GoNowGate::Source::Pad,true),"Go now was taken after the call ended");
 gate.Offer(call);
 for(std::size_t i=0;i<GoNowGate::MostRequests;++i)gate.Press(GoNowGate::Source::Keyboard,true);
 Check(!gate.Press(GoNowGate::Source::Keyboard,true),"A press past the bound was taken, and so kept from the game");
 gate.Take();
}
// The call's lifecycle sets the call and its go now as state the battle's
// countdown follows (TrainingSession.hxx: LeaveCountdown::Follow), so no
// part of the call is lost to a full command queue: a press taken for the
// call shortens that call's countdown once it runs, a call that ended stops
// it, a replacement starts a whole one, and a battle that has gone takes no
// press.
void CallLifecycleOrder(){
 using namespace sf4e::input;
 GoNowGate gate;CallLifecycle lifecycle;sf4e::training::LeaveCountdown countdown;
 CallBattle battle;battle.running=true;battle.generation=40;
 // One game tick then one battle update, as the game runs them.
 const auto tick=[&](const CallIdentity& call,bool fighting=true){
  const auto state=lifecycle.Tick(call,battle,gate);
  sf4e::training::CallControl control;control.call=state.call.serial;control.generation=state.call.generation;control.hurry=state.hurry;
  countdown.Follow(control,battle.generation,120);
  return countdown.Tick(fighting);
 };
 CallIdentity none,first;first.roomEpoch=7;first.table=1;first.opponent=2;first.generation=40;first.serial=1;
 Check(!gate.Press(GoNowGate::Source::Keyboard,true),"Go now was taken before the call stood");
 tick(first,false);
 Check(countdown.Left()==119&&countdown.Call()==1&&gate.Offered()==first,"The call did not start its countdown or was not offered");
 // A press taken for the call shortens its own countdown, and only once.
 Check(gate.Press(GoNowGate::Source::Keyboard,true),"Go now was not taken once the call was offered");
 Check(!tick(first,false)&&countdown.Left()==1,"Go now did not shorten its call's countdown");
 Check(tick(first)&&!tick(first)&&countdown.Left()==0,"Go now did not leave once");
 // Replaced by another opponent while hurried: the new call starts its own
 // whole count without the old call's go now, and a press for the old call
 // is dropped.
 sf4e::training::LeaveCountdown fresh;countdown=fresh;
 tick(first,false);gate.Press(GoNowGate::Source::Pad,true);tick(first,false);
 Check(countdown.Left()==1,"The first call's go now did not take");
 CallIdentity second=first;second.opponent=3;second.serial=2;
 gate.Press(GoNowGate::Source::Pad,true);
 Check(!tick(second,false)&&countdown.Left()==119&&countdown.Call()==2&&lifecycle.Dropped()==1,"A replacement call kept the old countdown or its go now");
 // Another room under the same seats: the same, as a new call.
 CallIdentity third=second;third.roomEpoch=8;third.serial=3;
 tick(third,false);
 Check(countdown.Left()==119&&countdown.Call()==3,"A new room's call kept the old countdown");
 // The call ends on the countdown's last frame: the next battle update
 // stops it before it could take the battle away.
 for(int frame=0;frame<200&&countdown.Left()>1;++frame)tick(third,false);
 Check(countdown.Left()==1,"The countdown did not reach its last frame");
 Check(!tick(none)&&countdown.Left()==0&&!tick(none),"A call that ended took the battle away");
 // The battle leaves, closes or another starts: nothing is offered, though the call stands.
 CallIdentity fourth=third;fourth.opponent=4;fourth.serial=4;
 for(const CallBattle gone:{CallBattle{false,40},CallBattle{true,41}}){
  lifecycle.Tick(fourth,gone,gate);
  Check(!gate.Offered().Live()&&!gate.Press(GoNowGate::Source::Keyboard,true),"Go now was offered for a battle that has gone");
 }
}
// View's go now is a pad action as the chord is: put to the call's gate only
// under the focus period its poll sampled, in the same step that checks it.
// A View sampled as the focus went, whether the focus is back yet or not,
// never hurries the call; a fresh press under the current period does, once.
void GoNowFocus(){
 using namespace sf4e::input;
 const float place[2]={0,0};
 for(const bool regain:{false,true}){
  TrainingControls controls;TrainingPadInput pad;GoNowGate gate;CallLifecycle lifecycle;sf4e::training::LeaveCountdown countdown;
  CallBattle battle;battle.running=true;battle.generation=40;
  CallIdentity a;a.roomEpoch=7;a.table=1;a.opponent=2;a.generation=40;a.serial=1;
  const auto tick=[&]{
   const auto state=lifecycle.Tick(a,battle,gate);
   sf4e::training::CallControl control;control.call=state.call.serial;control.generation=state.call.generation;control.hurry=state.hurry;
   countdown.Follow(control,battle.generation,120);
   return countdown.Tick(true);
  };
  tick();
  Check(gate.Offered()==a&&countdown.Left()==119,"The call was not offered");
  double now=0;
  // The poll samples its token, the focus goes (and comes back), and then View is read.
  const auto token=controls.Sample();
  controls.LoseFocus();if(regain)controls.GainFocus();
  PadOwner owner;owner.generation=40;owner.epoch=token.epoch;
  const auto stale=pad.Update(PhysicalBack,controls,now+=.02,TrainingCall::GoNow,owner,place);
  Check(stale.events.goNow,"View under the call did not ask for go now");
  Check(!controls.PressGoNow(stale.owner.epoch,a,gate,GoNowGate::Source::Pad,true)&&gate.Take().empty(),"A View sampled as the focus went was taken for go now");
  Check(!tick()&&countdown.Left()==118,"A View sampled as the focus went hurried the call");
  pad.Update(0,controls,now+=.02,TrainingCall::GoNow,owner,place);
  if(!regain)controls.GainFocus();
  owner.epoch=controls.Sample().epoch;
  const auto fresh=pad.Update(PhysicalBack,controls,now+=.02,TrainingCall::GoNow,owner,place);
  Check(fresh.events.goNow&&controls.PressGoNow(fresh.owner.epoch,a,gate,GoNowGate::Source::Pad,true),"A fresh View was not taken for go now");
  Check(tick()&&!tick()&&countdown.Left()==0,"A fresh go now did not leave once");
 }
}
// Behind another window go now comes through the same gesture and the same
// check: a fresh View under the call is taken without the focus; a View held
// as the focus went (and came back, and went again) between two polls is
// not; a View read under a call the room has since replaced is not; and the
// gesture's reset, save and controls are still refused there.
void GoNowBehind(){
 using namespace sf4e::input;
 const float place[2]={0,0};
 TrainingControls controls;TrainingPadInput pad;GoNowGate gate;
 CallIdentity a;a.roomEpoch=7;a.table=1;a.opponent=2;a.generation=40;a.serial=1;
 CallIdentity b=a;b.opponent=3;b.serial=2;
 double now=0;
 controls.LoseFocus();
 gate.Offer(a);
 PadOwner owner;owner.generation=40;owner.epoch=controls.Sample().epoch;
 pad.Update(0,controls,now+=.02,TrainingCall::GoNow,owner,place);
 const auto fresh=pad.Update(PhysicalBack,controls,now+=.02,TrainingCall::GoNow,owner,place);
 Check(fresh.events.goNow&&controls.PressGoNow(fresh.owner.epoch,a,gate,GoNowGate::Source::Pad,true),"A fresh View behind another window was not taken for go now");
 Check(gate.Take().size()==1,"Go now behind another window did not reach the call once");
 // Held: the focus comes and goes between two polls. The press keeps its old
 // owner, the gesture drops it, and the View still held is not go now.
 const auto heldOwner=controls.Sample().epoch;
 controls.GainFocus();controls.LoseFocus();
 owner.epoch=controls.Sample().epoch;
 const auto held=pad.Update(PhysicalBack,controls,now+=.02,TrainingCall::GoNow,owner,place);
 Check(!held.events.goNow&&heldOwner!=owner.epoch,"A View held across a focus change was taken as a fresh press");
 Check(!controls.PressGoNow(heldOwner,a,gate,GoNowGate::Source::Pad,true),"A View read under an ended focus period was taken");
 // The call is replaced between the poll that read View and the press.
 pad.Update(0,controls,now+=.02,TrainingCall::GoNow,owner,place);
 const auto read=pad.Update(PhysicalBack,controls,now+=.02,TrainingCall::GoNow,owner,place);
 gate.Offer(b);
 Check(read.events.goNow&&!controls.PressGoNow(read.owner.epoch,a,gate,GoNowGate::Source::Pad,true)&&gate.Take().empty(),
  "A View read under a call since replaced went now for its replacement");
 // Reset, save and the controls' chord ask nothing of an unfocused window.
 pad.Update(0,controls,now+=.02,TrainingCall::None,owner,place);
 pad.Update(PhysicalBack,controls,now+=.02,TrainingCall::None,owner,place);
 const auto chord=pad.Update(PhysicalBack|PhysicalStart,controls,now+=.02,TrainingCall::None,owner,place);
 Check(!chord.events.open&&!controls.Open(),"The controls opened behind another window");
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
// A board's rows move by where they are drawn (MenuNavigation::Place). The
// boxes are an Arcade room at 1.0x: two rows of eight cabinets, the matchup
// chips under them, the action bar under those, the members column on the
// right with the chat box level with the action bar. A row the board does not
// draw is not placed.
void BoardGeometry(){
 std::vector<MenuEntry> rows;std::vector<std::pair<std::string,MenuRect>> boxes;
 const auto add=[&](const std::string& id,MenuRect box){rows.push_back(Row(id,id,""));boxes.push_back({id,box});};
 for(int c=0;c<16;++c){const float x=20.f+(c%8)*110,y=100.f+(c/8)*130;add("cabinet-"+std::to_string(c),{x,y,x+100,y+120});}
 for(int k=0;k<4;++k)add("matchup-"+std::to_string(k),{20.f+k*220,370,230.f+k*220,410});
 for(int m=0;m<4;++m)add("member-"+std::to_string(m),{940,100.f+m*56,1200,148.f+m*56});
 add("room-chat",{940,430,1200,478});
 // The Members heading over the column, its Hide button inside it.
 add("room-members",{940,60,1200,92});add("room-hide-members",{1120,64,1196,88});
 add("ready",{20,430,200,478});add("room-training",{210,430,390,478});add("room-more",{680,430,790,478});add("leave",{800,430,900,478});
 rows.push_back(Row("unplaced","Not drawn",""));
 MenuNavigation nav;double time=0;
 const auto frame=[&](unsigned held){time+=.016;const auto a=nav.Update({held,time},rows);for(const auto& b:boxes)nav.Place(b.first,b.second);return a;};
 const auto press=[&](unsigned held){frame(held);frame(0);return nav.Focus();};
 const auto from=[&](const char* id,unsigned held){nav.Focus(id,rows);return press(held);};
 frame(0);frame(0);
 Check(from("cabinet-0",MenuInput::Left)=="cabinet-0"&&from("cabinet-8",MenuInput::Left)=="cabinet-8","Left off the board's edge moved");
 Check(from("cabinet-3",MenuInput::Up)=="cabinet-3"&&from("cabinet-7",MenuInput::Up)=="cabinet-7","Up from the top row reached the heading in the next column");
 Check(from("member-0",MenuInput::Up)=="room-members"&&from("room-members",MenuInput::Right)=="room-hide-members"&&from("room-hide-members",MenuInput::Down)=="member-0","The heading and the button inside it are not reached");
 Check(from("cabinet-2",MenuInput::Right)=="cabinet-3"&&from("cabinet-2",MenuInput::Down)=="cabinet-10","A cabinet's neighbours are not the ones beside and under it");
 Check(from("cabinet-7",MenuInput::Right)=="member-0","Right from the row's end did not reach the member level with it");
 Check(from("member-2",MenuInput::Left)=="cabinet-7"&&from("member-3",MenuInput::Left)=="cabinet-15","Left from a member did not reach the cabinet in its band");
 Check(from("cabinet-10",MenuInput::Down)=="matchup-1","Down from the bottom row did not reach the chip under it");
 Check(from("member-3",MenuInput::Down)=="room-chat"&&from("room-chat",MenuInput::Left)=="leave","The members column does not lead to the chat box and Leave room");
 Check(from("leave",MenuInput::Up)=="matchup-3"&&from("room-more",MenuInput::Right)=="leave","Up from Leave room is not the chip above it");
 nav.Focus("cabinet-6",rows);for(unsigned d:{MenuInput::Down,MenuInput::Down,MenuInput::Down,MenuInput::Right})press(d);
 Check(nav.Focus()=="leave","Cabinet 7 to Leave room is not four presses");
 // At the edge the focus stays (MenuEdge::Stay); with Flow it goes on to the
 // next or previous placed row in entry order, and stays past the ends.
 Check(nav.Edge()==MenuEdge::Stay&&from("room-more",MenuInput::Down)=="room-more","A board's edge did not keep the focus");
 nav.SetEdge(MenuEdge::Flow);
 Check(from("room-more",MenuInput::Down)=="leave"&&from("cabinet-8",MenuInput::Left)=="cabinet-7"&&from("cabinet-3",MenuInput::Up)=="cabinet-2","Flow did not go on in entry order");
 Check(from("cabinet-0",MenuInput::Left)=="cabinet-0"&&from("leave",MenuInput::Down)=="leave","Flow went past the first or last placed row");
 Check(from("cabinet-2",MenuInput::Down)=="cabinet-10","Flow overrode a row drawn that way");
 nav.SetEdge(MenuEdge::Stay);
 // An explicit neighbour still wins, a value row still adjusts, and a row
 // that is not placed walks the list.
 rows[0].right="leave";Check(from("cabinet-0",MenuInput::Right)=="leave","An explicit neighbour lost to the geometry");rows[0].right.clear();
 rows[20]=Value("member-0","Volume","3","");nav.Focus("member-0",rows);Check(frame(MenuInput::Right).kind==MenuAction::Adjust,"A placed value row stopped adjusting");frame(0);
 Check(from("unplaced",MenuInput::Up)=="leave","A row that is not drawn left the list's movement");
 // A screen that opens has no places until its board draws once: the list moves.
 nav.Push("list");nav.Focus("cabinet-0",rows);time+=.016;nav.Update({0,time},rows);time+=.016;nav.Update({MenuInput::Right,time},rows);
 Check(nav.Focus()=="cabinet-0","A list row moved sideways");time+=.016;nav.Update({0,time},rows);time+=.016;nav.Update({MenuInput::Down,time},rows);
 Check(nav.Focus()=="cabinet-1","A list without places did not walk its order");
 // The shell's footer (MenuEntry::footer) placed alone makes no board: the
 // list walks its order. One row a body places of its own is a board, whose
 // edge keeps the focus.
 std::vector<MenuEntry> list={Row("first","First",""),Row("second","Second",""),Row("retry","Retry","")};list[2].footer=true;
 const auto moved=[&](const char* placed,unsigned held){
  MenuNavigation n;double t=0;
  const auto tick=[&](unsigned h){t+=.016;n.Update({h,t},list);n.Place(placed,{0,0,100,40});};
  tick(0);tick(0);n.Focus(placed,list);tick(held);tick(0);return n.Focus();
 };
 Check(moved("retry",MenuInput::Up)=="second","A footer placed alone made the list a board");
 Check(moved("first",MenuInput::Down)=="first","A body's one placed row walked the list's order");
}
// Tabs wrap at both ends, from no tab too, and only an LB or RB Shortcut is a step.
void TabStepping(){
 Check(StepTab(5,1,6)==0&&StepTab(0,-1,6)==5&&StepTab(2,1,6)==3&&StepTab(2,-1,6)==1,"Tabs do not wrap");
 Check(StepTab(-1,1,3)==0&&StepTab(-1,-1,3)==2&&StepTab(0,1,0)==-1&&StepTab(0,1,1)==0,"A missing or single tab steps wrongly");
 Check(TabDelta({MenuAction::Shortcut,{},{},MenuInput::PrevTab})==-1&&TabDelta({MenuAction::Shortcut,{},{},MenuInput::NextTab})==1,"LB and RB are not tab steps");
 Check(TabDelta({MenuAction::Shortcut,"tab-about"})==0&&TabDelta({MenuAction::Shortcut,{},{},MenuInput::Fighter})==0&&TabDelta({MenuAction::Adjust,{},{},MenuInput::NextTab})==0,"Another action read as a tab step");
}
// A press that a toast answers is spent there: held on, it does not act on
// the screen once the toast lets go (MenuNavigation::Take).
void TakenPress(){
 MenuNavigation nav;double time=0;std::vector<MenuEntry> rows={Row("a","A","")};
 nav.Update({0,time},rows);nav.Update({0,time+=.016},rows);
 Check(nav.Take(MenuInput::Back)==MenuInput::Back,"The toast did not get the press");
 Check(nav.Update({MenuInput::Back,time+=.016},rows).kind==MenuAction::None&&nav.Update({MenuInput::Back,time+=.016},rows).kind==MenuAction::None,"A press the toast took went back too");
 Check(nav.Update({0,time+=.016},rows).kind==MenuAction::None&&nav.Update({MenuInput::Back,time+=.016},rows).kind==MenuAction::Close,"The next press did not go back");
}
void ScreenNames(){
 for(const char* screen:{"home","online","create","join","profile","main-character","selection","player","controllers","button-layout","button-layout-button","sent-reports","developer","discord-invitation","public-rooms","rankings","rankings-board","identity","identity-backup","linked-accounts","tournament-matches","tournament-desk","desk-event","desk-set","discord-connect","sign-in-site","assignment","about","room","room-table","room-members","room-member","room-chat","room-admin","stage","options","frame-data","hud","dummy","reply","tools","recording","history","recovery","updates","replays","report","awards","awards-icons","welcome"})
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
// The rendered dialogs' journeys (menu_dialog_test.cxx).
void DialogContract();
void HintPresses();
void DialogRepeat();
int main(){try{NativeReader();NavigationModel();NativeCapture();PracticeFightPresses();TrainingPadChord();TrainingPadQuick();TrainingPadEventOrder();TrainingPadOwnership();TrainingFlyoutOwner();GoNowRequests();CallLifecycleOrder();GoNowFocus();GoNowBehind();DialogContract();HintPresses();DialogRepeat();SelectPrecedence();BoardGeometry();TabStepping();TakenPress();ScreenNames();ProfileRecords();std::cout<<"Controller menu model, native reader/capture, and profile record journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
