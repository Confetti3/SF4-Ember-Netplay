#include "ControllerNavigation.hxx"
namespace sf4e { namespace ui {
void ControllerNavigation::Update(const ControllerSample& sample,bool available,bool visible,bool focused) {
    output_=0; backRequested_=focusRequested_=openRequested_=false;
    available_=available&&sample.connected;
    if(!available) { Reset(); return; }
    if(wasVisible_&&!visible)draining_=true;
    if(!sample.connected||sample.buttons==0)draining_=false;
    wasVisible_=visible;menuGuard_=visible||draining_;
    const bool changed=deviceType_!=sample.deviceType||deviceIndex_!=sample.deviceIndex;
    deviceType_=sample.deviceType;deviceIndex_=sample.deviceIndex;
    if(changed||!focused||!sample.connected)menuArmed_=false;
    else if(!sample.buttons)menuArmed_=true;
    else if(menuArmed_&&(sample.buttons&ControllerSample::Menu)){
        openRequested_=!visible;menuArmed_=false;
    }
    if(!visible||!focused||!sample.connected||changed){armed_=false;previous_=0;return;}
    if(!armed_){armed_=sample.buttons==0;previous_=0;return;}
    output_=sample.buttons&63;
    const auto pressed=output_&~previous_;previous_=output_;
    backRequested_=(pressed&ControllerSample::Back)!=0;
    focusRequested_=pressed!=0;
}
void ControllerNavigation::Reset(){*this=ControllerNavigation{};}
} }
