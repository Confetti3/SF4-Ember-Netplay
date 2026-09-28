#include "../ui/RecoveryController.hxx"
#include "../ui/MenuNavigation.hxx"
#include <iostream>
#include <stdexcept>
using namespace sf4e::ui;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
constexpr unsigned Up=MenuInput::Up,Down=MenuInput::Down,Left=MenuInput::Left,Right=MenuInput::Right,Select=MenuInput::Select,Back=MenuInput::Back;
constexpr std::uint16_t PadUp=0x0001,PadDown=0x0002,PadA=0x1000,PadB=0x2000,PadX=0x4000;
RecoveryPadSample Pad(std::size_t slot,std::uint16_t buttons=0,std::int16_t x=0,std::int16_t y=0){
    RecoveryPadSample s;s.pads[slot]={true,buttons,x,y};return s;
}
DirectInputStickSample Stick(std::uint64_t id,std::uint32_t pov=0xFFFFFFFFu,std::uint32_t buttons=0,bool axes=false,int x=0,int y=0){
    DirectInputStickSample s;s.id=id;s.pov=pov;s.buttons=buttons;s.axes=axes;s.x=x;s.y=y;return s;
}
RecoveryPadSample Sticks(std::initializer_list<DirectInputStickSample> sticks){RecoveryPadSample s;s.sticks=sticks;return s;}

void XInputSlots(){
    RecoveryControllerMap map;
    Check(map.Update(Pad(2))==0,"Idle slot 2 reported input");
    Check(map.Update(Pad(2,PadDown))==Down,"XInput slot 2 d-pad did not navigate");
    Check(map.Update(Pad(2,PadA))==Select&&map.Update(Pad(2,PadB))==Back,"XInput A/B are not Select/Back");
    Check(map.Update(Pad(2,PadX))==0,"XInput X leaked into the recovery menu");
    RecoveryControllerMap other;
    Check(other.Update(Pad(3))==0,"Idle slot 3 reported input");
    Check(other.Update(Pad(3,0,0,30000))==Up,"XInput slot 3 stick up did not navigate");
    Check(other.Update(Pad(3,0,-30000,0))==Left,"XInput slot 3 stick left did not navigate");
    Check(other.Update(Pad(3,0,7849,-7849))==0,"XInput stick inside its deadzone navigated");
    Check(other.Update(Pad(3))==0,"Released XInput pad still held a direction");
}
void DirectInputDirections(){
    RecoveryControllerMap map;
    Check(map.Update(Sticks({Stick(7)}))==0,"Centred stick reported input");
    Check(map.Update(Sticks({Stick(7,0)}))==Up&&map.Update(Sticks({Stick(7,9000)}))==Right&&
          map.Update(Sticks({Stick(7,18000)}))==Down&&map.Update(Sticks({Stick(7,27000)}))==Left,"POV cardinals mapped wrongly");
    Check(map.Update(Sticks({Stick(7,13500)}))==(Right|Down)&&map.Update(Sticks({Stick(7,31500)}))==(Left|Up),"POV diagonals lost a direction");
    Check(map.Update(Sticks({Stick(7,0x0000FFFFu)}))==0,"POV centred by its low word still navigated");
    Check(PovDirections(0xFFFFFFFFu)==0&&PovDirections(35999)==Up&&PovDirections(4500)==(Up|Right),"POV sectors wrong");
    RecoveryControllerMap axes;
    Check(axes.Update(Sticks({Stick(8,0xFFFFFFFFu,0,true)}))==0,"Centred axes reported input");
    Check(axes.Update(Sticks({Stick(8,0xFFFFFFFFu,0,true,-1000,0)}))==Left,"X axis left did not navigate");
    Check(axes.Update(Sticks({Stick(8,0xFFFFFFFFu,0,true,0,1000)}))==Down,"Y axis down did not navigate");
    Check(axes.Update(Sticks({Stick(8,0xFFFFFFFFu,0,true,0,-1000)}))==Up,"Y axis up did not navigate");
    Check(axes.Update(Sticks({Stick(8,0xFFFFFFFFu,0,true,400,-300)}))==0,"Axis inside the deadzone navigated");
    // Hysteresis: a direction taken at Press holds until the axis is back inside Release.
    Check(axes.Update(Sticks({Stick(8,0xFFFFFFFFu,0,true,600,0)}))==Right,"Axis past Press did not navigate");
    Check(axes.Update(Sticks({Stick(8,0xFFFFFFFFu,0,true,400,0)}))==Right,"Axis between thresholds chattered");
    Check(axes.Update(Sticks({Stick(8,0xFFFFFFFFu,0,true,300,0)}))==0,"Axis inside Release still held");
    Check(axes.Update(Sticks({Stick(8,0xFFFFFFFFu,0,false,-1000,-1000)}))==0,"A stick without axes read garbage axes");
}
void DirectInputButtons(){
    RecoveryControllerMap map;map.Update(Sticks({Stick(9)}));
    Check(map.Update(Sticks({Stick(9,0xFFFFFFFFu,1)}))==Select,"Button 1 is not Select");
    Check(map.Update(Sticks({Stick(9,0xFFFFFFFFu,2)}))==Back,"Button 2 is not Back");
    Check(map.Update(Sticks({Stick(9,0xFFFFFFFFu,4|8|0x80000000u)}))==0,"Other buttons leaked into the menu");
    Check(map.Update(Sticks({Stick(9,0xFFFFFFFFu,3)}))==(Select|Back),"Buttons 1 and 2 together lost one");
}
// An Xbox pad and a separate stick together: the poller keeps XInput pads out
// of DirectInput, so each is one device, and the glyphs follow whichever was
// pressed last, at once.
void XboxAndStick(){
    RecoveryControllerMap map;
    auto both=Pad(0);both.sticks={Stick(1)};
    Check(map.Update(both)==0,"An idle pad and stick reported input");
    auto pad=Pad(0,PadA);pad.sticks={Stick(1)};
    Check(map.Update(pad)==Select&&map.Family()==PadFamily::Xbox,"The pad's A was not Select with A/B glyphs");
    auto stick=Pad(0);stick.sticks={Stick(1,0xFFFFFFFFu,1)};
    Check(map.Update(stick)==Select&&map.Family()==PadFamily::DirectInput,"A stick press straight after the pad's was not its own");
    Check(map.Update(pad)==(Select)&&map.Family()==PadFamily::Xbox,"The pad did not take the glyphs back at once");
    Check(map.Update(both)==0,"Releasing both left bits held");
}
// Held input across focus: nothing while in the background, nothing on return
// until released, as the in-game ControllerNavigation does.
void FocusGate(){
    for(const auto held:{Select,Back,Down}){
        RecoveryControllerMap map;
        Check(map.Focus(0,true)==0&&map.Focus(held,true)==held,"Focused input did not pass");
        Check(map.Focus(held,false)==0,"Background input passed");
        Check(map.Focus(held,true)==0&&map.Focus(held,true)==0,"Input held across focus return pressed");
        Check(map.Focus(0,true)==0&&map.Focus(held,true)==held,"Input after release on return did not pass");
        // Minimized, or a native dialog up: no poll ran with active false,
        // but the window's own deactivation still disarms the pads.
        map.Focus(0,true);map.Deactivate();
        Check(map.Focus(held,true)==0,"Input held through a deactivation with no poll pressed on return");
        Check(map.Focus(0,true)==0&&map.Focus(held,true)==held,"Input after release following a deactivation did not pass");
    }
}
void GlyphFamily(){
    RecoveryControllerMap map;
    Check((map.Update({}),map.Family()==PadFamily::Keyboard),"No pads should show keyboard glyphs");
    Check((map.Update(Sticks({Stick(4)})),map.Family()==PadFamily::DirectInput),"An idle stick alone should show its button numbers");
    auto both=Pad(1);both.sticks={Stick(4)};
    Check((map.Update(both),map.Family()==PadFamily::Xbox),"An idle Xbox pad should show A/B before any press");
    auto stickPress=both;stickPress.sticks={Stick(4,0xFFFFFFFFu,1)};
    Check((map.Update(stickPress),map.Family()==PadFamily::DirectInput),"Pressing a stick did not switch to its glyphs");
    map.Update(both);
    auto key=both;key.keyboard=Down;
    Check((map.Update(key),map.Family()==PadFamily::Keyboard),"Pressing a key did not switch to keyboard glyphs");
    Check((map.Update(both),map.Family()==PadFamily::Keyboard),"Keyboard glyphs did not stick after the key was released");
    auto padPress=Pad(1,PadA);padPress.sticks={Stick(4)};
    Check((map.Update(padPress),map.Family()==PadFamily::Xbox),"Pressing the Xbox pad did not switch to A/B");
    map.Update(both);
    Check((map.Update(stickPress),map.Family()==PadFamily::DirectInput),"A stick press right after the pad's did not switch to its glyphs");
    Check((map.Update(Pad(1)),map.Family()==PadFamily::Xbox),"Unplugging the stick last used did not fall back to the Xbox pad");
    Check((map.Update({}),map.Family()==PadFamily::Keyboard),"Unplugging every pad did not fall back to the keyboard");
}
void ReleaseAndHotPlug(){
    RecoveryControllerMap map;
    auto all=Pad(2,PadDown|PadA,30000,0);all.sticks={Stick(5,9000,3,true,-1000,1000)};
    auto idle=Pad(2);idle.sticks={Stick(5,0xFFFFFFFFu,0,true)};
    map.Update(idle);
    Check(map.Update(all)!=0,"Held input was not reported");
    Check(map.Update(idle)==0,"Releasing everything left bits held");
    Check(map.Update({})==0,"Unplugging everything left bits held");
    // A device that appears already deflected (a stick whose axis rests off
    // centre, a button held through a hot-plug) waits for release, so it can
    // neither hold a direction nor keep MenuNavigation's neutral gate shut.
    auto resting=Sticks({Stick(6,0xFFFFFFFFu,0,true,-1000,0)});
    Check(map.Update(resting)==0&&map.Update(resting)==0,"An axis resting off centre held a direction");
    auto restingPlusPov=Sticks({Stick(6,18000,0,true,-1000,0)});
    Check(map.Update(restingPlusPov)==Down,"A resting axis also blocked the hat");
    Check(map.Update(Sticks({Stick(6,0xFFFFFFFFu,0,true)}))==0&&map.Update(resting)==Left,"A released axis stayed blocked");
    Check(map.Update(Pad(0,PadA))==0,"A button held through a hot-plug pressed Select");
    Check(map.Update(Pad(0))==0&&map.Update(Pad(0,PadA))==Select,"The hot-plugged pad did not work after release");
    MenuNavigation nav("recovery");const std::vector<MenuEntry> rows{{"a"},{"b"}};nav.NeutralGate();
    RecoveryControllerMap gate;
    for(int i=0;i<3;++i){MenuInput in;in.held=gate.Update(resting);nav.Update(in,rows);}
    auto press=resting;press.pads[1]={true,0,0,0};
    {MenuInput in;in.held=gate.Update(press);nav.Update(in,rows);}
    press.pads[1].buttons=PadDown;
    {MenuInput in;in.held=gate.Update(press);nav.Update(in,rows);}
    Check(nav.Focus()=="b","A resting stick kept the neutral gate shut");
}
}
int main(){
    try{
        XInputSlots();DirectInputDirections();DirectInputButtons();XboxAndStick();FocusGate();GlyphFamily();ReleaseAndHotPlug();
        std::cout<<"Recovery controller tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
