#include "sf4e__InputDevices.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace sf4e { namespace input {
namespace {
// Physical HID joysticks and gamepads by vendor:product. The device path also
// names the instance, so all that is kept from it is whether it is an XInput
// interface (IG_). Xbox 360 pads use a non-HID driver and appear only natively.
std::string HidControllers() {
    std::string result;
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || !count) return result;
    std::vector<RAWINPUTDEVICELIST> list(count);
    const UINT read = GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (read == UINT(-1)) return result;
    for (UINT i = 0; i < read; ++i) {
        if (list[i].dwType != RIM_TYPEHID) continue;
        RID_DEVICE_INFO info{}; info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) == UINT(-1)) continue;
        // Generic Desktop page: joystick (4) or gamepad (5).
        if (info.hid.usUsagePage != 1 || (info.hid.usUsage != 4 && info.hid.usUsage != 5)) continue;
        bool xinput = false;
        UINT chars = 0;
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, nullptr, &chars) == 0 && chars) {
            std::wstring path(chars, L'\0');
            if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, &path[0], &chars) != UINT(-1)) {
                for (auto& c : path) c = static_cast<wchar_t>(std::towupper(c));
                xinput = path.find(L"IG_") != std::wstring::npos;
            }
        }
        char entry[32];
        std::snprintf(entry, sizeof(entry), " %04X:%04X%s", static_cast<unsigned>(info.hid.dwVendorId & 0xFFFF),
            static_cast<unsigned>(info.hid.dwProductId & 0xFFFF), xinput ? "/xinput" : "");
        result += entry;
    }
    return result;
}
// What has to differ for the inventory to be logged again. ReadDevices runs
// every main-menu frame, so this decides between a quiet log and a flood.
std::string InventoryKey(const std::vector<Device>& devices) {
    // Pads only, held buttons never: the keyboard reports a button for any key.
    // A disconnect counts as a change. HID is read only when this changes, so a
    // pad the game cannot see is listed at the next change, not when plugged in.
    std::string key;
    for (const auto& device : devices) if (device.type != PadKeyboard)
        key += std::to_string(device.type) + ':' + std::to_string(device.index) + (device.connected ? '+' : '-') +
            device.name + '\n';
    return key;
}
// Whether a side of the game's pad system holds this device. The game keeps
// the keyboard in slot 0 or 1 and moves a player between them on a key press,
// while ReadDevices lists one keyboard, so a keyboard matches either slot.
bool Holds(int type,int index,const Device& device) {
    return type==device.type&&(type==Dimps::Pad::PADTYPE_RAWINPUT||index==device.index);
}
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
        if(Holds(type,index,device))native=device;
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
    if (Holds((pad->*methods.GetDeviceTypeForPlayer)(otherSide),
            (pad->*methods.GetDeviceIndexForPlayer)(otherSide), device)) ReleaseFromSide(otherSide);
    (pad->*methods.AssociatePlayerAndGamepad)(side, device.index);
    (pad->*methods.SetDeviceTypeForPlayer)(side, device.type);
    (pad->*methods.SetSideHasAssignedController)(side, 1);
    (pad->*methods.SetActiveButtonMapping)(fight ? System::BUTTON_MAPPING_FIGHT : System::BUTTON_MAPPING_MENU);
    return Holds((pad->*methods.GetDeviceTypeForPlayer)(side),(pad->*methods.GetDeviceIndexForPlayer)(side),device);
}
bool ReadAssignedInput(const Device& device,int side,unsigned& mapped,unsigned& raw) {
    mapped=raw=0;
    using namespace Dimps::Pad;
    if(side<0||side>1||!Connected(device))return false;
    auto* pad=System::staticMethods.GetSingleton();const auto& methods=System::publicMethods;
    if(!pad||!Holds((pad->*methods.GetDeviceTypeForPlayer)(side),(pad->*methods.GetDeviceIndexForPlayer)(side),device))
        return false;
    mapped=(pad->*methods.GetButtons_MappedOn)(side);raw=(pad->*methods.GetButtons_RawOn)(side);
    return true;
}
void LogInventory(const std::vector<Device>& devices) {
    // ReadDevices lists the keyboard once the provider is ready; before that
    // there is nothing to report. The first list is logged even with no pads.
    if (devices.empty()) return;
    static bool first = true;
    static std::string logged;
    std::string key = InventoryKey(devices);
    if (!first && key == logged) return;
    first = false;
    logged = key;
    std::string pads;
    for (const auto& device : devices) if (device.connected && device.type != PadKeyboard)
        pads += " [" + std::to_string(device.index) + (device.type == PadXInput ? " xinput '" : " dinput '") + device.name + "']";
    const std::string hid = HidControllers();
    // Steam's overlay loads even into a game Steam did not start, so this alone
    // does not mean Steam Input applies; Steam's virtual pad (28DE:11FF) in the
    // HID list does, and Steam then hides the real pad from the game.
    const bool steam = GetModuleHandleW(L"GameOverlayRenderer.dll") != nullptr;
    spdlog::info("Input: game pads{} | hid{} | steam overlay {}", pads.empty() ? " none" : pads,
        hid.empty() ? " none" : hid, steam ? "loaded" : "absent");
}
} }
