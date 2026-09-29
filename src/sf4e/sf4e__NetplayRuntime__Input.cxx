#include "sf4e__NetplayRuntime.hxx"

namespace sf4e { namespace NetplayFacade {
namespace internal {
// A device as the player reads it. Ember's own names for the keyboard and for a
// pad the driver did not name are localized; a name the driver reported is not
// ours to translate. Device::name itself stays as it is: connection checks
// compare it.
std::string DeviceName(const input::Device& device) {
    if (device.type == input::PadKeyboard) return loc::T("controller.keyboard");
    if (device.name == "Controller " + std::to_string(device.index + 1)) return loc::Tf("controller.numbered", device.index + 1);
    return device.name;
}
std::string ControllerLabel(const input::Device& device) {
    if (device.name.empty()) return loc::T("controller.none");
    return device.connected ? DeviceName(device) : loc::Tf("controller.disconnected_name", DeviceName(device));
}

void CaptureMenuInput() {
    if (AtMainMenu()) {
        runtime->inputDevices = input::ReadDevices();
        if (!runtime->inputInitialized && runtime->input.State()==input::Capture::Idle &&
            runtime->controller.GetSnapshot().room==netplay::RoomState::Idle) {
            auto device = input::MenuDevice(runtime->inputDevices);
            const auto& config = GetConfig();
            for (const auto& candidate : runtime->inputDevices)
                if (candidate.type == config.deviceType && candidate.index == config.deviceIdx) device = candidate;
            if (device.connected) {
                runtime->input.Adopt(device);
                // Keyboard fallback is provisional until explicitly chosen or
                // used to enter a room. A later connected pad can be adopted.
                runtime->inputInitialized=device.type!=Dimps::Pad::PADTYPE_RAWINPUT||config.deviceType==Dimps::Pad::PADTYPE_RAWINPUT;
                if(device.type!=Dimps::Pad::PADTYPE_RAWINPUT&&!input::AssignToSide(device,0,false))
                    runtime->input.Begin();
            } else if(!runtime->inputDevices.empty())runtime->input.Begin();
        }
        if (runtime->input.Tick(runtime->inputDevices)) {
            runtime->inputInitialized=true;
            if(!input::AssignToSide(runtime->input.Selected(),0,false))runtime->input.Begin();
        }
        if (UserApp::netplay && runtime->input.Ready()) {
            UserApp::netplay->deviceIdx = static_cast<uint8_t>(runtime->input.Selected().index);
            UserApp::netplay->deviceType = static_cast<uint8_t>(runtime->input.Selected().type);
        }
    } else runtime->input.Cancel();
}
} // namespace internal

bool BindRuntimeInput(int localSlot) {
    if (!runtime || !UserApp::netplay) return false;
    // Refresh connectivity just before native slot binding; never fall back to keyboard.
    runtime->input.Tick(input::ReadDevices());
    if (!runtime->input.Ready()) { runtime->error = loc::T("runtime.gameplay_device_disconnected"); return false; }
    const auto& device = runtime->input.Selected();
    UserApp::netplay->deviceIdx = static_cast<uint8_t>(device.index);
    UserApp::netplay->deviceType = static_cast<uint8_t>(device.type);
    if(!input::AssignToSide(device, localSlot, true)) {
        runtime->error=loc::T("runtime.controller_assignment_unverified");return false;
    }
    runtime->matchInput=device;runtime->matchInputSide=localSlot;runtime->matchInputFault=false;
    return true;
}

bool ReadRuntimeMatchInput(int side,unsigned& mapped,unsigned& raw) {
    mapped=raw=0;
    if(!runtime||!runtime->attached)return false;
    const bool valid=side==runtime->matchInputSide&&input::ReadAssignedInput(runtime->matchInput,side,mapped,raw);
    runtime->matchInputFault=!valid;
    return valid;
}

} } // namespace sf4e::NetplayFacade
