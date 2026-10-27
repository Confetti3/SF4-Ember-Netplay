#include "sf4e__NetplayRuntime.hxx"
#include "../netplay/ControllerRestore.hxx"
#include "sf4e__Gamepads.hxx"
#include "sf4e__DirectInputFilter.hxx"
#include "sf4e__Overlay.hxx"
#include "sf4e__SelfTest.hxx"
#include <ctime>
#include "../ui/ButtonLayoutScreen.hxx"

namespace sf4e { namespace NetplayFacade {
namespace internal {
// A device as the player reads it. Ember's own names for the keyboard and for a
// pad the driver did not name are localized; a name the driver reported is not
// ours to translate. Device::name itself stays as it is: connection checks
// compare it.
std::string DeviceName(const input::Device& device) {
    if (device.type == input::PadKeyboard) return loc::T("controller.keyboard");
    if (device.type == input::PadXInput && device.index >= 0 && device.index < 4) {
        const auto identity = Gamepads::IdentityForSlot(device.index);
        if (identity && (device.identity.guid.empty() || gamepads::Same(*identity, device.identity))) return identity->name;
    }
    if (!device.identity.guid.empty()) return device.identity.name;
    if (device.name == "Controller " + std::to_string(device.index + 1)) return loc::Tf("controller.numbered", device.index + 1);
    return device.name;
}
std::string ControllerLabel(const input::Device& device) {
    if (device.name.empty()) return loc::T("controller.none");
    return device.connected ? DeviceName(device) : loc::Tf("controller.disconnected_name", DeviceName(device));
}

void LogGameplayController(const input::Device& device, const char* reason) {
    spdlog::info("Input: gameplay controller '{}' id {:08x} slot {} by {}", DeviceName(device),
        gamepads::IdentityHash(device.identity), device.index, reason);
}

namespace {
// Runs every tick while the saved controller is missing or only stood in for.
void RestoreMenuController() {
    OverlayPrefs::Data prefs;
    OverlayPrefs::LoadEachTick(prefs);
    input::Device saved;
    saved.type = prefs.deviceType; saved.index = prefs.deviceIdx;
    if (prefs.deviceIdentity) saved.identity = *prefs.deviceIdentity;
    const auto restored = input::RestoreController(runtime->inputDevices, input::MenuDevice(runtime->inputDevices), saved,
        runtime->input.Selected());
    const auto& device = restored.device;
    runtime->inputInitialized = !restored.provisional && device.connected;
    if (!device.connected) {
        if (!runtime->inputDevices.empty()) runtime->input.Begin();
        return;
    }
    const bool migration = !prefs.deviceIdentity && prefs.deviceType == input::PadXInput &&
        !device.identity.guid.empty() && !restored.provisional;
    if (!restored.provisional && !device.identity.guid.empty())
        OverlayPrefs::SaveGameplayDevice(device.type, device.index, device.identity);
    const auto& previous = runtime->input.Selected();
    if (previous.connected && previous.Same(device)) return;
    if (prefs.deviceIdentity) {
        if (restored.provisional) {
            runtime->padHint = device.type == input::PadKeyboard ? std::string{} :
                loc::Tf("controller.using_for_now", prefs.deviceIdentity->name, DeviceName(device));
            spdlog::info("Input: saved controller id {:08x} not connected; using '{}' for now",
                gamepads::IdentityHash(*prefs.deviceIdentity), DeviceName(device));
        } else {
            if (previous.type >= 0) runtime->padHints.Adopted(device.identity,
                Overlay::MenuPads().Takes() ? gamepads::HintContext::OpenMainMenu : gamepads::HintContext::HiddenMainMenu, GetTickCount64());
            spdlog::info("Input: saved controller '{}' id {:08x} restored to slot {}",
                DeviceName(device), gamepads::IdentityHash(device.identity), device.index);
        }
        runtime->padHintUntil = GetTickCount64() + 4000;
    }
    runtime->input.Adopt(device);
    LogGameplayController(device, migration ? "migration" : restored.provisional && device.type != input::PadKeyboard ? "only pad" : "restore");
    if (device.type != input::PadKeyboard && !input::AssignToSide(device, 0, false)) runtime->input.Begin();
}
}

void CaptureMenuInput() {
    if (GetTickCount64() >= runtime->padHintUntil) runtime->padHint.clear();
    if (AtMainMenu()) {
        // The keyboard's held bit is read only by a capture waiting for every
        // key to be released. Pad rows never show the keyboard, so the
        // Controllers page has no use for it either.
        const bool keyboardScanned = runtime->input.State() != input::Capture::Idle;
        runtime->inputDevices = input::ReadDevices(keyboardScanned);
        input::LogInventory(runtime->inputDevices);
        const auto setup = Gamepads::ReadFrame().setup;
        if (!setup.state.active && setup.outcome != gamepads::SetupEnd::Saved) runtime->setupAdoption.reset();
        if (runtime->setupAdoption && !setup.state.active && setup.outcome == gamepads::SetupEnd::Saved) {
            const auto device = std::find_if(runtime->inputDevices.begin(), runtime->inputDevices.end(), [&](const input::Device& d) {
                return d.connected && d.type == input::PadXInput && !d.identity.guid.empty() && gamepads::Same(d.identity, *runtime->setupAdoption);
            });
            if (device != runtime->inputDevices.end() && input::AssignToSide(*device, 0, false)) {
                runtime->input.Cancel(); runtime->input.Adopt(*device); runtime->inputInitialized = true;
                OverlayPrefs::SaveGameplayDevice(device->type, device->index, device->identity);
                LogGameplayController(*device, "restore"); runtime->setupAdoption.reset();
            }
        }
        // A DirectInput pad in setup reads disconnected to the game (LendToSetup);
        // a provisional choice is not replaced or captured over meanwhile.
        if (!runtime->inputInitialized && !setup.state.active && runtime->input.State()==input::Capture::Idle &&
            runtime->controller.GetSnapshot().room==netplay::RoomState::Idle) RestoreMenuController();
        // RestoreMenuController can begin a capture after the read above.
        if (!keyboardScanned && runtime->input.State() != input::Capture::Idle) input::ScanKeyboard(runtime->inputDevices);
        const int previousIndex = runtime->input.Selected().index;
        const bool captured = runtime->input.Tick(runtime->inputDevices);
        const auto& chosen = runtime->input.Selected();
        const bool moved = runtime->input.Ready() && chosen.index != previousIndex &&
            runtime->controller.GetSnapshot().room == netplay::RoomState::Idle;
        if (captured || moved) {
            if (captured) { runtime->padHint.clear(); runtime->padHints.Selected(chosen.identity); }
            if (captured) runtime->inputInitialized=true;
            if (runtime->inputInitialized) {
                const auto identity = chosen.identity.guid.empty() ? std::nullopt : std::make_optional(chosen.identity);
                OverlayPrefs::SaveGameplayDevice(chosen.type, chosen.index, identity);
            }
            LogGameplayController(chosen, captured ? "capture" : "restore");
            if(!input::AssignToSide(runtime->input.Selected(),0,false))runtime->input.Begin();
        }
        if (UserApp::netplay && runtime->input.Ready()) {
            UserApp::netplay->deviceIdx = static_cast<uint8_t>(runtime->input.Selected().index);
            UserApp::netplay->deviceType = static_cast<uint8_t>(runtime->input.Selected().type);
        }
    } else runtime->input.Cancel();
    if (!AtMainMenu() || runtime->controller.GetSnapshot().room != netplay::RoomState::Idle) Gamepads::ControlSetup(gamepads::SetupCommand::LeftMenu, 0);
    const auto layer = Gamepads::ReadFrame();
    if (layer.mode == gamepads::Layer::Active) runtime->preferences.buttonLayouts = layer.layouts;
    runtime->padLayer = layer.mode; runtime->runningControllerLayer = layer.requested;
    const auto savedIdentity = OverlayPrefs::DeviceIdentity();
    const auto native = DirectInputFilter::ReadControllers();
    runtime->pads = runtime->padInventory.Build(layer.mode, layer.pads, runtime->inputDevices, native, runtime->input.Selected(), savedIdentity);
    for (auto& pad : runtime->pads) {
        if (pad.state == gamepads::RowState::Absent) {
            const auto match = std::find_if(runtime->preferences.buttonLayouts.begin(), runtime->preferences.buttonLayouts.end(), [&](const gamepads::StoredLayout& layout) { return gamepads::Same(layout.identity, pad.identity); });
            if (match != runtime->preferences.buttonLayouts.end()) { pad.layout = match->buttons; pad.table = match->buttons; pad.bound = match->bound; pad.extras = match->extras; }
        }
        pad.buttonNames = ui::LayoutButtonNames(pad, runtime->preferences.buttonPrompts);
        if (pad.name.empty() && pad.state == gamepads::RowState::Steam) pad.name = loc::T("controllers.steam_name");
        if (pad.state == gamepads::RowState::GameRead && pad.slot >= 0)
            for (const auto& device : runtime->inputDevices) if (device.type == input::PadXInput && device.index == pad.slot) pad.name = DeviceName(device);
    }
    for (auto& pad : runtime->pads) {
        if (pad.state == gamepads::RowState::Absent) continue;
        if (pad.slot >= 0) pad.menuButtons = MenuButtonsFor(input::PadXInput, pad.slot);
        else if (pad.directInputIndex >= 0) pad.menuButtons = MenuButtonsFor(input::PadDirectInput, pad.directInputIndex);
    }
    const auto context = !AtMainMenu() ? gamepads::HintContext::NativeMenu :
        runtime->controller.GetSnapshot().room != netplay::RoomState::Idle ? gamepads::HintContext::Room :
        Overlay::MenuPads().Takes() ? gamepads::HintContext::OpenMainMenu : gamepads::HintContext::HiddenMainMenu;
    const auto hints = runtime->padHints.Update(runtime->pads, runtime->inputInitialized && runtime->input.Selected().type == input::PadKeyboard, context, GetTickCount64());
    for (const auto& hint : hints) {
        runtime->padHint = loc::Tf(gamepads::PadHintKey(hint.kind), hint.name);
        runtime->padHintUntil = GetTickCount64() + 4000;
    }
    const auto& selected = runtime->input.Selected();
    const auto brand = gamepads::ResolvePrompts(runtime->preferences.buttonPrompts, selected.type, Gamepads::BrandForDevice(selected.type, selected.index));
    if (selected.type == input::PadXInput && (!runtime->loggedPromptBrand || !(*runtime->loggedPromptBrand == brand) ||
        runtime->loggedButtonPrompts != runtime->preferences.buttonPrompts)) {
        spdlog::info("Input: prompts {} ({})", gamepads::BrandName(brand.brand == gamepads::Brand::Generic ? gamepads::Brand::Xbox : brand.brand),
            runtime->preferences.buttonPrompts == gamepads::ButtonPrompts::Automatic ? "automatic" : "forced");
        runtime->loggedPromptBrand = brand; runtime->loggedButtonPrompts = runtime->preferences.buttonPrompts;
    }
}

bool CanChooseControllerNow() {
    const auto current = runtime->controller.GetSnapshot();
    return AtMainMenu() && CurrentGates().canChangeController && !runtime->readyIntent.Parked() &&
        !current.readyPending && (current.match == netplay::MatchState::None || current.match == netplay::MatchState::PostMatch);
}

namespace {
// The pad the layer serves on an XInput slot: only such a pad carries an Ember
// layout, and so bound buttons. Null for any other device.
const gamepads::PadView* LayerPad(int deviceType, int deviceIndex) {
    const auto pad = std::find_if(runtime->pads.begin(), runtime->pads.end(), [&](const gamepads::PadView& pad) {
        return deviceType == input::PadXInput && pad.slot == deviceIndex &&
            (pad.state == gamepads::RowState::Recognised || pad.state == gamepads::RowState::Steam);
    });
    return pad == runtime->pads.end() ? nullptr : &*pad;
}
}

input::MenuButtons MenuButtonsFor(int deviceType, int deviceIndex) {
    const auto* pad = LayerPad(deviceType, deviceIndex);
    if (!pad) return Dimps::Pad::MenuButtons(deviceType, deviceIndex);
    using namespace gamepads;
    return Dimps::Pad::MenuButtons(deviceType, deviceIndex, LayoutButtonMask(pad->bound[MenuConfirmButton], pad->has),
        LayoutButtonMask(pad->bound[MenuBackButton], pad->has));
}

input::TrainingButtons TrainingButtonsFor(int deviceType, int deviceIndex) {
    const auto* pad = LayerPad(deviceType, deviceIndex);
    input::TrainingButtons buttons;
    if (!pad) return buttons;
    using namespace gamepads;
    buttons.record = LayoutButtonMask(pad->bound[TrainingRecordButton], pad->has);
    buttons.play = LayoutButtonMask(pad->bound[TrainingPlayButton], pad->has);
    if (buttons.record) buttons.recordName = pad->buttonNames[pad->bound[TrainingRecordButton]];
    if (buttons.play) buttons.playName = pad->buttonNames[pad->bound[TrainingPlayButton]];
    return buttons;
}

bool CanEditButtonLayoutNow() {
    const auto gates = CurrentGates();
    return AtMainMenu() && !netplay::Fighting(runtime->controller.GetSnapshot().match) &&
        (gates.canEditPreferences || runtime->controller.GetSnapshot().room != netplay::RoomState::Idle);
}

void HandleControllerRequest(const gamepads::ControllerRequest& request) {
    using Kind = gamepads::ControllerRequest::Kind;
    if (request.kind == Kind::LayoutLog) { spdlog::info("Input: {}", request.log); return; }
    if (request.kind == Kind::SaveLayouts) {
        if (!CanEditButtonLayoutNow()) return;
        auto preferences = runtime->preferences;
        preferences.buttonLayouts = request.layouts;
        if (!preferences.Valid()) return;
        if (OverlayPrefs::SavePlayerPreferences(preferences)) {
            runtime->preferences = std::move(preferences);
            Gamepads::SetLayouts(runtime->preferences.buttonLayouts);
        } else runtime->error = loc::T("runtime.preferences_queue_failed");
        return;
    }
    if (request.kind == Kind::DeclinePrompt) { spdlog::info("Input: setup prompt declined"); return; }
    if (request.kind == Kind::SetupControl) {
        const auto current = Gamepads::ReadFrame().setup;
        if (!current.state.active || current.padId != request.id) return;
        if (request.command != gamepads::SetupCommand::Save) { Gamepads::ControlSetup(request.command, request.step); return; }
        if (!AtMainMenu() || runtime->controller.GetSnapshot().room != netplay::RoomState::Idle || !CurrentGates().canEditPreferences) {
            Gamepads::ControlSetup(gamepads::SetupCommand::LeftMenu, 0); return;
        }
        const auto before = Gamepads::ReadFrame().setup;
        if (const auto mapping = Gamepads::SaveSetup()) {
            runtime->preferences.controllerMappings[before.identity.guid] = *mapping;
            OverlayPrefs::SavePlayerPreferences(runtime->preferences);
            if (runtime->setupAdoption) runtime->setupAdoption = before.identity;
        }
        return;
    }
    const auto found = std::find_if(runtime->pads.begin(), runtime->pads.end(), [&](const gamepads::PadView& pad) { return pad.id == request.id; });
    if (found == runtime->pads.end()) return;
    const auto& pad = *found;
    const auto gates = CurrentGates();
    if (request.kind == Kind::Layout || request.kind == Kind::RemoveLayout) {
        if (!CanEditButtonLayoutNow() || runtime->padLayer != gamepads::Layer::Active ||
            (pad.state != gamepads::RowState::Recognised && pad.state != gamepads::RowState::Steam)) return;
        if (request.kind == Kind::Layout) {
            if (!gamepads::ValidLayout(request.buttons) || !gamepads::ValidBoundButtons(request.bound) || !gamepads::ValidExtraCopies(request.extras)) return;
            const auto dropped = gamepads::UpsertLayout(runtime->preferences.buttonLayouts,
                {pad.identity, request.buttons, static_cast<std::int64_t>(std::time(nullptr)), request.bound, request.extras});
            if (dropped) spdlog::info("Input: layout for id {} dropped, oldest of 33", gamepads::IdentityHash(*dropped));
        } else gamepads::RemoveLayout(runtime->preferences.buttonLayouts, pad.identity);
        Gamepads::SetLayouts(runtime->preferences.buttonLayouts);
        if (!request.log.empty()) spdlog::info("Input: {}", request.log);
        return;
    }
    if (request.kind == Kind::Prompt) { Gamepads::SetupPrompt(pad.identity); return; }
    if (request.kind == Kind::Setup) {
        if (AtMainMenu() && runtime->controller.GetSnapshot().room == netplay::RoomState::Idle && gates.canEditPreferences && runtime->padLayer == gamepads::Layer::Active &&
            (pad.state == gamepads::RowState::Recognised || pad.state == gamepads::RowState::Unmapped || pad.state == gamepads::RowState::GameRead))
        {
            if (Gamepads::ReadFrame().setup.state.active) return;
            runtime->setupAdoption = pad.gameplay ? std::make_optional(pad.identity) : std::nullopt;
            Gamepads::BeginSetup(pad.identity, pad.id);
        }
        return;
    }
    if (request.kind == Kind::Forget) {
        if (!gates.canEditPreferences || runtime->padLayer != gamepads::Layer::Active || !runtime->preferences.controllerMappings.count(pad.identity.guid)) return;
        auto preferences = runtime->preferences; preferences.controllerMappings.erase(pad.identity.guid);
        if (!OverlayPrefs::SavePlayerPreferences(preferences)) return;
        runtime->preferences = std::move(preferences);
        spdlog::info("Input: setup for guid {} forgotten; applies next start", pad.identity.guid);
        runtime->padHint = loc::T("controllers.forgotten"); runtime->padHintUntil = GetTickCount64() + 4000;
        return;
    }
    if (request.kind != Kind::Play || !CanChooseControllerNow() || pad.gameplay ||
        (pad.state != gamepads::RowState::Recognised && pad.state != gamepads::RowState::Steam)) return;
    const auto device = std::find_if(runtime->inputDevices.begin(), runtime->inputDevices.end(), [&](const input::Device& d) {
        return d.connected && d.type == input::PadXInput && !d.identity.guid.empty() && gamepads::Same(d.identity, pad.identity);
    });
    if (device == runtime->inputDevices.end()) return;
    runtime->setupAdoption.reset();
    runtime->input.Cancel(); runtime->input.Adopt(*device); runtime->inputInitialized = true;
    if (!input::AssignToSide(*device, 0, false)) { runtime->input.Begin(); return; }
    runtime->padHints.Selected(device->identity);
    OverlayPrefs::SaveGameplayDevice(device->type, device->index, device->identity);
    LogGameplayController(*device, "list");
    runtime->padHint = loc::Tf("controllers.now_playing", pad.name); runtime->padHintUntil = GetTickCount64() + 4000;
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
    spdlog::info("Input: P{} plays with {} {} '{}'", localSlot + 1,
        device.type == input::PadKeyboard ? "keyboard" : device.type == input::PadXInput ? "xinput" : "dinput",
        device.index, device.identity.guid.empty() ? device.name : device.identity.name);
    return true;
}

bool ReadRuntimeMatchInput(int side,unsigned& mapped,unsigned& raw) {
    mapped=raw=0;
    if(!runtime||!runtime->attached)return false;
    const bool valid=side==runtime->matchInputSide&&input::ReadAssignedInput(runtime->matchInput,side,mapped,raw);
    runtime->matchInputFault=!valid;
    // The tournament self-test plays its own fighter; nothing without SF4E_SELFTEST.
    selftest::MatchInput(side,mapped,raw);
    return valid;
}

} } // namespace sf4e::NetplayFacade
