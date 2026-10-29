#include "ControllerNavigation.hxx"
namespace sf4e { namespace ui {
void ControllerNavigation::Update(const ControllerSample& sample,bool available,bool visible,bool enabled) {
    output_=0; backRequested_=focusRequested_=false;
    available_=available&&sample.connected;
    if(!available) { Reset(); return; }
    if(wasVisible_&&!visible)draining_=true;
    if(!sample.connected||sample.buttons==0)draining_=false;
    wasVisible_=visible;menuGuard_=visible||draining_;
    const bool changed=deviceType_!=sample.deviceType||deviceIndex_!=sample.deviceIndex;
    deviceType_=sample.deviceType;deviceIndex_=sample.deviceIndex;
    if(!visible||!enabled||!sample.connected||changed){gate_.Rearm();return;}
    output_=gate_.Held(sample.buttons & ~ControllerSample::Menu);
    const auto pressed=gate_.Pressed(output_);
    backRequested_=(pressed&ControllerSample::Back)!=0;
    focusRequested_=pressed!=0;
}
void ControllerNavigation::Reset(){*this=ControllerNavigation{};}
} }
