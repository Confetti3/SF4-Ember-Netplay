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
 // The pad drives the main menu, the training controls and a menu opened over an export, nothing else.
 Check(ControllerMenuAvailable(MenuContext::MainMenu)&&ControllerMenuAvailable(MenuContext::OfflineTraining)&&
  ControllerMenuAvailable(MenuContext::ReplayExport)&&!ControllerMenuAvailable(MenuContext::Unavailable),"Controller navigation escaped its contexts");
 // During an export the pad drives a menu F10 opened, but Start stays the game's pause.
 Check(!sf4e::input::ControllerOpensMenu(MenuContext::ReplayExport)&&
  sf4e::input::ControllerOpensMenu(MenuContext::MainMenu)&&!sf4e::input::ControllerOpensMenu(MenuContext::OfflineTraining),"Start opened Ember over an export");
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
// The pad in offline Training: Back tapped resets, held saves, and Start
// pressed while Back is held opens the controls or closes them. None of the
// three clashes with another: the chord never resets or saves, a hold saves
// once, and the game's pause (Start first) never opens or resets.
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
 TrainingPadQueue queue;
 const auto event=[&](Kind kind,std::uint64_t generation,float x=0){TrainingPadEvent e;e.kind=kind;e.generation=generation;e.epoch=queue.Epoch();e.place[0]=x;return e;};
 Check(queue.Post(event(Kind::Save,4,120))&&queue.Post(event(Kind::Reset,4))&&queue.Post(event(Kind::Save,4,200)),"The queue refused a gesture");
 auto taken=queue.Take();
 Check(taken.size()==3&&taken[0].kind==Kind::Save&&taken[0].place[0]==120&&taken[1].kind==Kind::Reset&&taken[2].kind==Kind::Save&&taken[2].place[0]==200,
  "Gestures between frames were merged or reordered, or a save lost where its press was");
 Check(queue.Take().empty(),"An event was taken twice");
 for(std::size_t i=0;i<TrainingPadQueue::MostEvents;++i)queue.Post(event(Kind::Reset,4));
 Check(!queue.Post(event(Kind::Save,4))&&queue.Take().size()==TrainingPadQueue::MostEvents,"A full queue took one more or merged it");
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
 TrainingPadInput pad;TrainingFlyout flyout;double now=0;
 const float place[2]={100,300};
 PadOwner owner;owner.generation=4;owner.epoch=1;
 const auto step=[&](unsigned physical,const PadOwner& current,double seconds=1.0/60){now+=seconds;return pad.Update(physical,flyout,now,TrainingCall::None,current,place);};
 const unsigned back=PhysicalBack,both=PhysicalBack|PhysicalStart;
 auto r=step(back,owner);
 Check(r.events.pressed&&r.owner==owner&&r.place[0]==100,"A press did not take its owner and place");
 PadOwner regained=owner;regained.epoch=2;
 r=step(both,regained);
 Check(!r.events.open&&!flyout.Open(),"A chord finished under a new pad owner opened the controls");
 Check(!step(0,regained).events.Any(),"A press from the old owner reset or saved");
 r=step(back,regained);step(both,regained);
 Check(flyout.Open(),"A fresh chord under the new owner did not open the controls");
 step(0,regained);flyout.Set(false);
 // Another battle while Back is held: no save comes of the old press.
 step(back,regained);PadOwner next=regained;next.generation=5;
 Check(!step(back,next,1).events.save&&!step(0,next).events.Any(),"A press from the last battle saved in the next");
 r=step(back,next);r=step(back,next,.6);
 Check(r.events.save&&r.owner==next,"A hold under one owner did not save for it");
 step(0,next);
 // A batch taken just before the owner changed is not applied.
 TrainingPadQueue queue;TrainingPadEvent event;event.kind=TrainingPadEvent::Kind::Save;event.generation=5;event.epoch=queue.Epoch();
 Check(queue.Post(event),"The queue refused an event of its owner");
 const auto taken=queue.Take();queue.Invalidate();
 Check(taken.size()==1&&!queue.Current(taken[0]),"An event taken before the owner changed was still current");
}
// The controls' open state has one owner. Two chords before a frame open and
// then close them, from either state; F6 and the controls' own Back act on
// the same state, and a chord whose Back already closed the controls (a
// DirectInput pad's Back is the menu's too) does not open them again.
void TrainingFlyoutOwner(){
 using namespace sf4e::input;
 PadOwner owner;owner.generation=1;owner.epoch=1;const float place[2]={0,0};
 for(const bool start:{false,true}){
  TrainingPadInput pad;TrainingFlyout flyout;flyout.Set(start);double now=0;
  const auto step=[&](unsigned physical){now+=1.0/60;return pad.Update(physical,flyout,now,TrainingCall::None,owner,place);};
  for(int chord=0;chord<2;++chord){step(PhysicalBack);step(PhysicalBack|PhysicalStart);step(0);}
  Check(flyout.Open()==start,"Two chords before a frame did not open and close the controls");
  step(PhysicalBack);step(PhysicalBack|PhysicalStart);step(0);
  Check(flyout.Open()==!start,"One chord did not change the controls");
  // F6 between chords: the next chord acts on what F6 left.
  flyout.Set(!flyout.Open());
  step(PhysicalBack);step(PhysicalBack|PhysicalStart);step(0);
  Check(flyout.Open()==!start,"A chord after F6 did not act on what F6 left");
 }
 TrainingPadInput pad;TrainingFlyout flyout;flyout.Set(true);double now=0;
 const auto step=[&](unsigned physical){now+=1.0/60;return pad.Update(physical,flyout,now,TrainingCall::None,owner,place);};
 step(PhysicalBack);flyout.Set(false);// the controls' own Back closed them
 step(PhysicalBack|PhysicalStart);step(0);
 Check(!flyout.Open(),"The chord reopened controls its own Back had closed");
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
// The call's lifecycle drives the battle's countdown (TrainingSession.hxx:
// LeaveCountdown) in order: a press is never taken before its call's
// countdown is queued, always shortens that call's own countdown, and a call
// replaced by another, or a battle that has gone, takes no press.
void CallLifecycleOrder(){
 using namespace sf4e::input;
 GoNowGate gate;CallLifecycle lifecycle;sf4e::training::LeaveCountdown countdown;
 bool full=false;std::vector<CallOrder> orders;
 const auto order=[&](CallOrder kind,const CallIdentity& call){
  if(full)return false;
  orders.push_back(kind);
  if(kind==CallOrder::Leave)countdown.Start(120,call.serial);
  else if(kind==CallOrder::Stay)countdown.Cancel(call.serial);
  else countdown.Hurry(call.serial);
  return true;
 };
 CallBattle battle;battle.running=true;battle.generation=40;
 CallIdentity none,first;first.roomEpoch=7;first.table=1;first.opponent=2;first.generation=40;first.serial=1;
 // The first tick of the call: an Enter before it is the game's, its
 // countdown is queued before the call is offered, and a press after it
 // shortens that countdown on the next tick.
 Check(!gate.Press(GoNowGate::Source::Keyboard,true),"Go now was taken before the call's countdown was queued");
 lifecycle.Tick(none,first,false,battle,gate,order);
 Check(orders==std::vector<CallOrder>{CallOrder::Leave}&&countdown.Left()==120&&gate.Offered()==first,"The call was offered before its countdown, or not at all");
 Check(gate.Press(GoNowGate::Source::Keyboard,true),"Go now was not taken once the call was offered");
 lifecycle.Tick(first,first,false,battle,gate,order);
 Check(orders.back()==CallOrder::LeaveNow&&countdown.Left()==1&&countdown.Call()==1,"Go now did not shorten its call's countdown");
 // Replaced by another opponent while hurried: the old count ends, the new
 // call starts its own whole, and a press made for the old call is dropped.
 CallIdentity second=first;second.opponent=3;second.serial=2;
 gate.Press(GoNowGate::Source::Pad,true);
 orders.clear();lifecycle.Tick(first,second,false,battle,gate,order);
 Check(orders==std::vector<CallOrder>({CallOrder::Stay,CallOrder::Leave})&&countdown.Left()==120&&countdown.Call()==2,"A replacement call kept the old countdown or its hurry");
 Check(lifecycle.Dropped()==1&&gate.Offered()==second,"A press for the replaced call went now");
 // Another room under the same seats: the same, as a new call.
 CallIdentity third=second;third.roomEpoch=8;third.serial=3;
 countdown.Hurry(2);
 orders.clear();lifecycle.Tick(second,third,false,battle,gate,order);
 Check(orders==std::vector<CallOrder>({CallOrder::Stay,CallOrder::Leave})&&countdown.Left()==120&&countdown.Call()==3,"A new room's call kept the old countdown");
 // A start the battle's queue could not take is asked again, and the call
 // is not offered until it is queued.
 CallIdentity fourth=third;fourth.opponent=4;fourth.serial=4;
 full=true;lifecycle.Tick(third,fourth,false,battle,gate,order);
 Check(!gate.Offered().Live()&&!gate.Press(GoNowGate::Source::Keyboard,true),"A call was offered before its countdown was queued");
 full=false;orders.clear();lifecycle.Tick(fourth,fourth,false,battle,gate,order);
 Check(orders==std::vector<CallOrder>{CallOrder::Leave}&&gate.Offered()==fourth,"A refused start was not asked again");
 // The battle leaves, closes or another starts: nothing is offered, though the call stands.
 for(const CallBattle gone:{CallBattle{false,40},CallBattle{true,41}}){
  lifecycle.Tick(fourth,fourth,false,gone,gate,order);
  Check(!gate.Offered().Live()&&!gate.Press(GoNowGate::Source::Keyboard,true),"Go now was offered for a battle that has gone");
 }
 // Reaching the main menu ends the call without staying.
 orders.clear();lifecycle.Tick(fourth,none,true,battle,gate,order);
 Check(orders.empty()&&!gate.Offered().Live(),"The call's arrival was taken as it ending early");
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
 // The opening press can arrive with pointer movement over the action button.
 // Preserve Cancel until a later movement deliberately selects that button.
 imgui.io.AddMousePosEvent(1,1);frame();frame();
 const auto confirmAt=centres.at("confirm/1");
 imgui.io.AddMousePosEvent(confirmAt.x,confirmAt.y);frame(MenuInput::Select);frame();
 Check(menu.navigation.Confirming()&&!menu.navigation.ConfirmSelected(),"Opening pointer movement replaced the safe confirmation default");
 const auto declined=press(MenuInput::Select);
 Check(declined.kind==MenuAction::None&&!menu.navigation.Confirming(),"Default confirmation activated the action");
 press(MenuInput::Select);frame();
 Check(menu.navigation.Confirming()&&!menu.navigation.ConfirmSelected(),"Reopened confirmation inherited Send or a resting pointer");
 imgui.io.AddMousePosEvent(confirmAt.x+2,confirmAt.y);frame();
 Check(menu.navigation.ConfirmSelected(),"Pointer movement after opening did not select the action");
 const auto confirmed=press(MenuInput::Select);
 Check(confirmed.kind==MenuAction::Activate&&confirmed.id=="leave","Explicit pointer selection did not confirm the action");
 imgui.io.AddMousePosEvent(1,1);frame();
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
 // A notice over an editor owns Enter; the editor resumes with its draft.
 rows={TextRow("name","Name","Kate",31)};menu.navigation.Focus("name",rows);
 imgui.io.AddMousePosEvent(1,1);frame();press(MenuInput::Select);frame();
 menu.ShowNotice("Advice");frame();frame();
 imgui.io.AddKeyEvent(ImGuiKey_Enter,true);frame();
 Check(!menu.NoticeOpen()&&menu.navigation.Editing()&&menu.navigation.Draft()=="Kate","Enter did not dismiss only the editor's notice");
 imgui.io.AddKeyEvent(ImGuiKey_Enter,false);frame();frame();
 // Single-line editors still accept either Enter key, even with Cancel lit.
 for(const auto enter:{ImGuiKey_Enter,ImGuiKey_KeypadEnter}) {
  menu.navigation.EditAccepts(false);imgui.io.AddKeyEvent(enter,true);const auto accepted=frame();
  Check(accepted.kind==MenuAction::TextAccepted&&accepted.text=="Kate"&&!menu.navigation.Editing(),"Single-line Enter did not accept the draft");
  imgui.io.AddKeyEvent(enter,false);frame();frame();
  if(enter==ImGuiKey_Enter){press(MenuInput::Select);frame();}
 }
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
 for(const char* screen:{"home","online","create","join","profile","main-character","selection","settings","player","defaults","interface","training-replays","problem-reports","sent-reports","developer",
   "discord","discord-invitation","public-rooms","identity","identity-backup","linked-accounts","tournament-matches","discord-connect","assignment","about","room","room-table","room-members","room-member","room-chat","room-admin",
   "roster","appearance","costumes","colors","ultra","stage","options","frame-data","dummy","reply","tools","recording","history","recovery","updates","replays","report"})
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
int main(){try{NativeReader();NavigationModel();NativeCapture();TrainingPadChord();TrainingPadEventOrder();TrainingPadOwnership();TrainingFlyoutOwner();GoNowRequests();CallLifecycleOrder();DialogContract();SelectPrecedence();FlyoutIgnoresChoices();ScreenNames();ProfileRecords();std::cout<<"Controller menu model, native reader/capture, and profile record journeys passed.\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
