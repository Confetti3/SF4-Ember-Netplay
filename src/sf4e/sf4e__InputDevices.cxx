#include "sf4e__InputDevices.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include <algorithm>

namespace sf4e { namespace input {
namespace {
bool Connected(const Device& device) {
    using namespace Dimps::Pad;
    if(!device.connected)return false;
    if(device.type==PADTYPE_RAWINPUT)return device.index==0;
    if(device.index<0||device.index>=12||device.type!=(device.index<4?PADTYPE_XINPUT:PADTYPE_DIRECTINPUT))return false;
    auto* backend=System_XInput::staticMethods.GetSingleton();
    const auto& methods=System_XInput::publicMethods;
    if(!backend||device.index>=(backend->*methods.GetDeviceCount)()||
        (backend->*methods.GetDeviceStatus)(device.index)!=1)return false;
    const char* name=(backend->*methods.GetDeviceName)(device.index);
    return name&&*name?device.name==name:device.name=="Controller "+std::to_string(device.index+1);
}
}
std::vector<Device> ReadDevices() {
    using namespace Dimps::Pad;
    std::vector<Device> result;
    auto* pad = System::staticMethods.GetSingleton();
    auto* controllers = System_XInput::staticMethods.GetSingleton();
    if (!pad || !controllers) return result;
    Device keyboard; keyboard.type = PADTYPE_RAWINPUT; keyboard.index = 0;
    keyboard.name = "Keyboard"; keyboard.connected = true;
    // Ignore mouse buttons; pressing Change controller must not select a mouse.
    for (int key = VK_BACK; key < 256; ++key)
        if (GetAsyncKeyState(key) & 0x8000) keyboard.buttons = 1;
    result.push_back(keyboard);
    const auto& methods = System_XInput::publicMethods;
    // This native backend owns XInput indices 0..3 AND DirectInput indices 4+.
    // See docs/validation/CONTROLLER_SELECTION_VALIDATION.md for the native call evidence.
    const int count = (std::min)(12, (controllers->*methods.GetDeviceCount)());
    for (int index = 0; index < count; ++index) {
        Device device; device.index = index;
        device.type = index < 4 ? PADTYPE_XINPUT : PADTYPE_DIRECTINPUT;
        device.connected = (controllers->*methods.GetDeviceStatus)(index) == 1;
        if (device.connected) {
            const char* name = (controllers->*methods.GetDeviceName)(index);
            device.name = name && *name ? name : "Controller " + std::to_string(index + 1);
            device.buttons = (controllers->*methods.GetButtonsOn)(index);
        }
        result.push_back(std::move(device));
    }
    return result;
}
Device MenuDevice(const std::vector<Device>& devices) {
    using namespace Dimps::Pad;
    auto* pad = System::staticMethods.GetSingleton();
    if (!pad) return {};
    const auto& methods = System::publicMethods;
    const int type = (pad->*methods.GetDeviceTypeForPlayer)(0);
    const int index = (pad->*methods.GetDeviceIndexForPlayer)(0);
    Device native,only;unsigned controllers=0;
    for(const auto& device:devices)if(device.connected){
        if(device.type==type&&device.index==index)native=device;
        if(device.type==PADTYPE_XINPUT||device.type==PADTYPE_DIRECTINPUT){only=device;++controllers;}
    }
    if(native.connected&&native.type!=PADTYPE_RAWINPUT)return native;
    if(controllers==1)return only;
    // Multiple pads need release/press/release capture; enumeration order is
    // not evidence of the user's intended controller.
    return controllers?Device{}:native;
}
void ReleaseFromSide(int side) {
    using namespace Dimps::Pad;
    if (side < 0 || side > 1) return;
    auto* pad = System::staticMethods.GetSingleton();
    if (!pad) return;
    const auto& methods = System::publicMethods;
    const int type = (pad->*methods.GetDeviceTypeForPlayer)(side);
    const int index = (pad->*methods.GetDeviceIndexForPlayer)(side);
    (pad->*methods.SetSideHasAssignedController)(side, 0);
    if (index < 0 || index > 254) return;
    if (type == PADTYPE_RAWINPUT) {
        auto* raw = System_RawInput::staticMethods.GetSingleton();
        if (raw) (raw->*System_RawInput::publicMethods.SetDeviceInUse)(index, 0);
    } else if (type == PADTYPE_XINPUT || type == PADTYPE_DIRECTINPUT) {
        auto* controllers = System_XInput::staticMethods.GetSingleton();
        if (controllers && index < (controllers->*System_XInput::publicMethods.GetDeviceCount)())
            (controllers->*System_XInput::publicMethods.SetDeviceInUse)(index, 0);
    }
}
bool AssignToSide(const Device& device, int side, bool fight) {
    using namespace Dimps::Pad;
    if (side < 0 || side > 1 || !Connected(device)) return false;
    auto* pad = System::staticMethods.GetSingleton();
    if (!pad) return false;
    const auto& methods = System::publicMethods;
    ReleaseFromSide(side);
    const int otherSide = 1 - side;
    if ((pad->*methods.GetDeviceTypeForPlayer)(otherSide) == device.type &&
        (pad->*methods.GetDeviceIndexForPlayer)(otherSide) == device.index) ReleaseFromSide(otherSide);
    (pad->*methods.AssociatePlayerAndGamepad)(side, device.index);
    (pad->*methods.SetDeviceTypeForPlayer)(side, device.type);
    (pad->*methods.SetSideHasAssignedController)(side, 1);
    (pad->*methods.SetActiveButtonMapping)(fight ? System::BUTTON_MAPPING_FIGHT : System::BUTTON_MAPPING_MENU);
    return (pad->*methods.GetDeviceTypeForPlayer)(side)==device.type&&
        (pad->*methods.GetDeviceIndexForPlayer)(side)==device.index;
}
bool ReadAssignedInput(const Device& device,int side,unsigned& mapped,unsigned& raw) {
    mapped=raw=0;
    using namespace Dimps::Pad;
    if(side<0||side>1||!Connected(device))return false;
    auto* pad=System::staticMethods.GetSingleton();const auto& methods=System::publicMethods;
    if(!pad||(pad->*methods.GetDeviceTypeForPlayer)(side)!=device.type||
        (pad->*methods.GetDeviceIndexForPlayer)(side)!=device.index)return false;
    mapped=(pad->*methods.GetButtons_MappedOn)(side);raw=(pad->*methods.GetButtons_RawOn)(side);
    return true;
}
} }
