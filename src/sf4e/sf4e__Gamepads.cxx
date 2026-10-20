#include "sf4e__Gamepads.hxx"
#include "sf4e__GamepadBindings.hxx"
#include "sf4e__GamepadLayouts.hxx"
#include "sf4e__ImportPatch.hxx"
#include "sf4e__DirectInputFilter.hxx"
#include "sf4e__OverlayPrefs.hxx"
#include "../common/XInputMapping.hxx"
#include "../common/GamepadRumble.hxx"
#include "../netplay/SettingsStore.hxx"
#include "../netplay/ControllerMappings.hxx"
#include <windows.h>
#include <Xinput.h>
#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>
#include <map>
#include <deque>
#include <mutex>
#include <shared_mutex>

namespace sf4e { namespace Gamepads {
namespace {
using namespace gamepads;
struct Snapshot {
    Id id = 0;
    DWORD packet = 0;
    XInputFields fields;
};
// The provider polls in the game-thread device loop (System_XInput::Update).
// Overlay readers can run on the rendering thread; the same lock also makes
// import callbacks and rumble safe if another native caller uses them there.
std::shared_mutex mutex;
bool requested = false, started = false, active = false, logReady = false;
bool layerSetting = true;
std::string startupNote;
bool startupWarning = false;
std::optional<Identity> preferred;
std::set<std::string> savedEmberMappingGuids;
// Of those, the GUIDs SDL also maps itself. Ember's mapping replaces SDL's, so
// this is noted before Ember adds one.
std::set<std::string> sdlMappedGuids;
PadSetup setupModel;
SetupSnapshot setup;
std::deque<std::pair<SetupCommand, int>> setupCommands;
bool setupRestLogged = false;
Slots slots;
Inventory inventory, logged;
bool haveLogged = false;
std::map<Id, SDL_Gamepad*> handles;
struct RawJoystick { SDL_Joystick* handle; std::vector<int> rest; };
std::map<Id, RawJoystick> rawHandles;
std::vector<PadFrame> frames;
std::vector<Identity> rumbleUnsupported;
struct RumbleState {
    WORD left = 0, right = 0;
    bool pending = false;
    DWORD result = ERROR_SUCCESS;
};
std::map<Id, RumbleState> rumble;
std::array<Snapshot, 4> snapshots;
std::atomic<void*> originalState{nullptr}, originalCapabilities{nullptr}, originalRumble{nullptr}, originalBattery{nullptr};

std::string Text(const char* value) { return value ? value : ""; }

const char* TypeName(Type type) {
    switch (type) {
    case Type::Standard: return "standard";
    case Type::Xbox360: return "xbox360";
    case Type::XboxOne: return "xboxone";
    case Type::PS3: return "ps3";
    case Type::PS4: return "ps4";
    case Type::PS5: return "ps5";
    case Type::SwitchPro: return "switchpro";
    case Type::JoyConLeft: return "joyconleft";
    case Type::JoyConRight: return "joyconright";
    case Type::JoyConPair: return "joyconpair";
    case Type::GameCube: return "gamecube";
    default: return "unknown";
    }
}

// The mapping SDL holds for a GUID (its own, or an Ember one once added), or empty.
std::string MappingForGuid(const std::string& guid) {
    char* mapping = SDL_GetGamepadMappingForGUID(SDL_StringToGUID(guid.c_str()));
    auto text = Text(mapping); SDL_free(mapping);
    return text;
}

std::string IdentityTag(const Identity& identity) {
    return fmt::format("{:08x}", IdentityHash(identity));
}

void LogLocked() {
    if (!logReady) return;
    if (!startupNote.empty()) {
        if (startupWarning) spdlog::warn("{}", startupNote);
        else spdlog::info("{}", startupNote);
        startupNote.clear();
    }
    if (!active || (haveLogged && inventory == logged)) return;
    for (const auto& old : logged.pads) if (old.slot >= 0 && std::none_of(inventory.pads.begin(), inventory.pads.end(), [&](const Pad& pad) { return pad.id == old.id; }))
        spdlog::info("Input: pad removed '{}' id {} slot {} reserved", old.identity.name, IdentityTag(old.identity), old.slot);
    std::string line;
    for (const auto& pad : inventory.pads) {
        const auto old = std::find_if(logged.pads.begin(), logged.pads.end(), [&](const Pad& old) { return old.id == pad.id; });
        if (old == logged.pads.end()) {
            if (pad.slot >= 0) {
                const auto brand = DetectBrand(pad).brand;
                spdlog::info("Input: pad added '{}' {:04X}:{:04X} type {} brand {} guid {} id {} slot {}", pad.identity.name,
                    pad.vid, pad.pid, TypeName(pad.type), BrandName(brand), pad.identity.guid, IdentityTag(pad.identity), pad.slot);
            } else if (pad.status == Status::Unmapped)
                spdlog::info("Input: pad added '{}' {:04X}:{:04X} guid {} id {} unmapped, left to DirectInput", pad.identity.name,
                    pad.vid, pad.pid, pad.identity.guid, IdentityTag(pad.identity));
            else if (pad.status == Status::NotInUse)
                spdlog::info("Input: pad added '{}' id {} not in use, four slots taken", pad.identity.name, IdentityTag(pad.identity));
        } else if (old->status == Status::NotInUse && pad.slot >= 0)
            spdlog::info("Input: '{}' id {} takes freed slot {}", pad.identity.name, IdentityTag(pad.identity), pad.slot);
        if (!line.empty()) line += " | ";
        line += pad.status == Status::Slotted ? "slot " + std::to_string(pad.slot) :
            pad.status == Status::Unmapped ? "unmapped" : pad.status == Status::GameRead ? "game-read" :
            pad.status == Status::SteamTwin ? "Steam twin" : "not in use";
        line += fmt::format(" '{}' {} {:04X}:{:04X}", pad.identity.name, TypeName(pad.type), pad.vid, pad.pid);
        if (SteamVirtual(pad) && pad.slot >= 0)
            spdlog::info("Input: Steam virtual pad '{}' as {} slot {}", pad.identity.name, TypeName(pad.type), pad.slot);
        if (pad.steamPad) {
            const auto steam = std::find_if(inventory.pads.begin(), inventory.pads.end(),
                [&](const Pad& candidate) { return candidate.id == pad.steamPad; });
            if (steam != inventory.pads.end())
                spdlog::info("Input: physical pad '{}' id {} paired with Steam pad slot {}; not opened",
                    pad.identity.name, IdentityTag(pad.identity), steam->slot);
        }
    }
    spdlog::info("Input: controller layer {}", line.empty() ? "no pads" : line);
    logged = inventory;
    haveLogged = true;
}

void Rebuild() {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) { spdlog::warn("Input: controller inventory failed (SDL: {})", SDL_GetError()); return; }
    std::vector<Pad> pads;
    for (int i = 0; i < count; ++i) {
        Pad pad;
        pad.id = ids[i];
        pad.vid = SDL_GetJoystickVendorForID(pad.id); pad.pid = SDL_GetJoystickProductForID(pad.id);
        pad.type = PadType(pad.id);
        pad.mapped = SDL_IsGamepad(pad.id);
        pad.arcadeStick = SDL_GetJoystickTypeForID(pad.id) == SDL_JOYSTICK_TYPE_ARCADE_STICK;
        char guid[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(pad.id), guid, sizeof(guid));
        pad.identity = {guid, "", Text(SDL_GetJoystickPathForID(pad.id)), Text(SDL_GetJoystickNameForID(pad.id))};
        pad.sdlMapping = savedEmberMappingGuids.count(pad.identity.guid) ? sdlMappedGuids.count(pad.identity.guid) != 0 : pad.mapped;
        pads.push_back(std::move(pad));
    }
    SDL_free(ids);
    Classify(pads, savedEmberMappingGuids);
    const auto settingUp = setup.state.active ? std::make_optional(setup.identity) : std::nullopt;
    for (auto it = rawHandles.begin(); it != rawHandles.end();) {
        const bool keep = std::any_of(pads.begin(), pads.end(), [&](const Pad& pad) {
            return pad.id == it->first && WantsRawHandle(pad, settingUp);
        });
        if (keep) { ++it; continue; }
        SDL_CloseJoystick(it->second.handle); it = rawHandles.erase(it);
    }
    // The game's device lets go before SDL opens the pad, so SDL's open acquires
    // it and reads its rest; closed handles above hand it back to the game.
    std::vector<std::string> lent;
    for (const auto& pad : pads) if (WantsRawHandle(pad, settingUp)) lent.push_back(pad.identity.path);
    DirectInputFilter::LendToSetup(lent);
    for (auto& pad : pads) if (WantsRawHandle(pad, settingUp) && !rawHandles.count(pad.id)) {
        if (auto* joystick = SDL_OpenJoystick(pad.id)) {
            RawJoystick raw{joystick, {}};
            for (int axis = 0; axis < SDL_GetNumJoystickAxes(joystick); ++axis) {
                Sint16 rest = 0;
                if (!SDL_GetJoystickAxisInitialState(joystick, axis, &rest)) rest = SDL_GetJoystickAxis(joystick, axis);
                raw.rest.push_back(rest);
            }
            rawHandles.emplace(pad.id, std::move(raw));
        }
    }
    // SDL's DirectInput backend reports no serial, so an unmapped pad's identity
    // is the same with or without its setup handle.
    for (auto& pad : pads) {
        const auto raw = rawHandles.find(pad.id);
        if (raw != rawHandles.end()) pad.identity.serial = Text(SDL_GetJoystickSerial(raw->second.handle));
    }
    for (auto it = handles.begin(); it != handles.end();) {
        const bool keep = std::any_of(pads.begin(), pads.end(), [&](const Pad& pad) {
            return pad.id == it->first && pad.status == Status::NotInUse;
        });
        if (keep) { ++it; continue; }
        SDL_CloseGamepad(it->second);
        it = handles.erase(it);
    }
    for (auto& pad : pads) if (pad.status == Status::NotInUse) {
        auto handle = handles.find(pad.id);
        if (handle == handles.end()) {
            if (auto* opened = SDL_OpenGamepad(pad.id)) handle = handles.emplace(pad.id, opened).first;
            else spdlog::warn("Input: controller '{}' not in use (SDL: {})", pad.identity.name, SDL_GetError());
        }
        pad.available = handle != handles.end();
        if (pad.available) pad.identity.serial = Text(SDL_GetGamepadSerial(handle->second));
    }
    inventory = slots.Update(std::move(pads), preferred, savedEmberMappingGuids);
    for (const auto& dropped : slots.DroppedReservations()) {
        const auto pad = std::find_if(inventory.pads.begin(), inventory.pads.end(),
            [&](const Pad& candidate) { return candidate.id == dropped.replacement; });
        spdlog::info("Input: slot {} reserved for id {} given to '{}' id {}", dropped.slot,
            IdentityTag(dropped.owner), pad->identity.name, IdentityTag(pad->identity));
    }
}

// SDL's buttons for the extra buttons, in Extra order.
constexpr std::array<SDL_GamepadButton, ExtraCount> ExtraButtons = {
    SDL_GAMEPAD_BUTTON_TOUCHPAD, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_MISC1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2};
State ReadState(SDL_Gamepad* pad) {
    constexpr std::array<SDL_GamepadButton, static_cast<unsigned>(Button::Count)> buttons = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT
    };
    State state;
    for (unsigned i = 0; i < buttons.size(); ++i) state.buttons[i] = SDL_GetGamepadButton(pad, buttons[i]);
    for (unsigned i = 0; i < ExtraCount; ++i) state.extras[i] = SDL_GetGamepadButton(pad, ExtraButtons[i]);
    state.leftX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    state.leftY = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    state.rightX = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    state.rightY = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
    state.leftTrigger = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    state.rightTrigger = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    return state;
}

RawState ReadRaw(SDL_Joystick* joystick, const std::vector<int>& rest = {}) {
    RawState state; state.rest = rest;
    for (int i = 0; i < SDL_GetNumJoystickButtons(joystick); ++i) state.buttons.push_back(SDL_GetJoystickButton(joystick, i));
    for (int i = 0; i < SDL_GetNumJoystickHats(joystick); ++i) state.hats.push_back(SDL_GetJoystickHat(joystick, i));
    for (int i = 0; i < SDL_GetNumJoystickAxes(joystick); ++i) state.axes.push_back(SDL_GetJoystickAxis(joystick, i));
    return state;
}
void UpdateSetup(const RawState& raw, bool connected) {
    if (!setup.state.active) return;
    std::vector<SetupEvent> events;
    while (!setupCommands.empty()) {
        const auto command = setupCommands.front(); setupCommands.pop_front();
        const auto output = setupModel.Update(GetTickCount64(), raw, connected, command.first, command.second);
        events.insert(events.end(), output.begin(), output.end());
    }
    const auto output = setupModel.Update(GetTickCount64(), raw, connected);
    events.insert(events.end(), output.begin(), output.end());
    setup.state = setupModel.State();
    if (!setupRestLogged && connected && !setup.state.releaseButtons && setupModel.Rest().size() == raw.axes.size()) {
        std::string rest;
        for (unsigned i = 0; i < setupModel.Rest().size(); ++i) {
            if (!rest.empty()) rest += ",";
            rest += fmt::format("a{}={}", i, setupModel.Rest()[i]);
        }
        spdlog::info("Input: setup started for '{}' guid {}: {} buttons, {} hats, {} axes; rest {}",
            setup.identity.name, setup.identity.guid, raw.buttons.size(), raw.hats.size(), raw.axes.size(), rest);
        setupRestLogged = true;
    }
    for (const auto& event : events) {
        const auto name = SetupSteps[event.step].name;
        switch (event.kind) {
        case SetupEventKind::Ignored: spdlog::info("Input: setup ignores held {}", RawBinding(*event.input)); break;
        case SetupEventKind::Record: spdlog::info("Input: setup step {} = {}", name, RawBinding(*event.input)); break;
        case SetupEventKind::Skip: spdlog::info("Input: setup step {} = skipped", name); break;
        case SetupEventKind::Back: spdlog::info("Input: setup step {} = back", name); break;
        case SetupEventKind::Conflict: spdlog::info("Input: setup step {} = conflict with {}", name, SetupSteps[event.other].name); break;
        case SetupEventKind::Cancel: case SetupEventKind::Stopped:
            spdlog::info("Input: setup cancelled ({})", SetupEndName(event.reason));
            setup.outcome = event.reason; ++setup.revision; break;
        default: break;
        }
    }
}
void ApplyRumble() {
    for (auto it = rumble.begin(); it != rumble.end();) {
        const auto handle = handles.find(it->first);
        if (handle == handles.end() || !SDL_GamepadConnected(handle->second)) { it = rumble.erase(it); continue; }
        auto& request = it->second;
        const bool applied = ShouldApplyRumble(request.pending, request.left, request.right) &&
            SDL_RumbleGamepad(handle->second, request.left, request.right, 200);
        if (request.pending) {
            request.pending = false;
            const bool supported = SDL_GetBooleanProperty(SDL_GetGamepadProperties(handle->second), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
            request.result = !applied && supported ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS;
            if (!applied && !supported) {
                const auto pad = std::find_if(inventory.pads.begin(), inventory.pads.end(), [&](const Pad& pad) { return pad.id == it->first; });
                if (pad != inventory.pads.end() && std::none_of(rumbleUnsupported.begin(), rumbleUnsupported.end(), [&](const Identity& identity) { return Same(identity, pad->identity); })) {
                    rumbleUnsupported.push_back(pad->identity);
                    spdlog::info("Input: rumble unsupported on '{}'", pad->identity.name);
                }
            }
        }
        if (!request.left && !request.right) it = rumble.erase(it);
        else ++it;
    }
}
void UpdateLocked(bool rebuild) {
    SDL_UpdateGamepads();
    SDL_PumpEvents();
    SDL_Event events[32];
    int count;
    while ((count = SDL_PeepEvents(events, 32, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0)
        for (int i = 0; i < count; ++i) switch (events[i].type) {
        case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_REMOVED:
        case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_REMOVED: case SDL_EVENT_GAMEPAD_REMAPPED:
            rebuild = true;
        }
    if (rebuild) Rebuild();
    GamepadLayouts::Update(inventory);
    std::array<Snapshot, 4> next{};
    frames.clear();
    RawState setupRaw; bool setupConnected = false;
    for (const auto& pad : inventory.pads) {
        PadFrame frame; frame.pad = pad;
        GamepadLayouts::Read(frame, inventory);
        const bool settingUp = setup.state.active && Same(pad.identity, setup.identity);
        auto handle = handles.find(pad.id);
        if (handle != handles.end() && SDL_GamepadConnected(handle->second)) {
            frame.state = ReadState(handle->second);
            if (pad.hasEmberMapping) {
                // A failed read names no bound inputs this frame.
                frame.bindings = ReadBindings(handle->second).value_or(std::vector<SdlBinding>{});
                frame.numberedHats = SDL_GetNumJoystickHats(SDL_GetGamepadJoystick(handle->second)) > 1;
            }
            const SDL_GamepadButton buttons[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
            for (unsigned i = 0; i < 6; ++i) frame.has[i] = SDL_GamepadHasButton(handle->second, buttons[i]);
            frame.has[6] = SDL_GamepadHasAxis(handle->second, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            frame.has[7] = SDL_GamepadHasAxis(handle->second, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
            frame.has[8] = SDL_GamepadHasButton(handle->second, SDL_GAMEPAD_BUTTON_LEFT_STICK);
            frame.has[9] = SDL_GamepadHasButton(handle->second, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            for (unsigned i = 0; i < ExtraCount; ++i) frame.hasExtras[i] = SDL_GamepadHasButton(handle->second, ExtraButtons[i]);
            const auto held = [](bool down) { return down; };
            frame.pressed = std::any_of(frame.state.buttons.begin(), frame.state.buttons.end(), held) ||
                std::any_of(frame.state.extras.begin(), frame.state.extras.end(), held) ||
                frame.state.leftTrigger > 16383 || frame.state.rightTrigger > 16383;
            if (pad.slot >= 0) {
                next[pad.slot].id = pad.id;
                // The layout's extra button copies reach what the game reads, not the held glyphs.
                next[pad.slot].fields = settingUp ? XInputFields{} : MapToXInput(WithExtraCopies(frame.state, frame.extras));
            }
        }
        RawState rawState;
        const auto raw = rawHandles.find(pad.id);
        if (raw != rawHandles.end()) {
            auto state = ReadRaw(raw->second.handle, raw->second.rest);
            if (settingUp) rawState = state;
            frame.raw = HeldRaw(state);
            frame.rawAtRest = HeldRaw(state, 8000).empty();
            frame.pressed = !frame.raw.empty();
        }
        if (settingUp) {
            setup.identity = pad.identity;
            if (handle != handles.end()) rawState = ReadRaw(SDL_GetGamepadJoystick(handle->second));
            setupRaw = std::move(rawState);
            setupConnected = (handle != handles.end() && SDL_GamepadConnected(handle->second)) ||
                (raw != rawHandles.end() && SDL_JoystickConnected(raw->second.handle));
        }
        frames.push_back(std::move(frame));
    }
    const bool wasSetupActive = setup.state.active;
    UpdateSetup(setupRaw, setupConnected);
    if (wasSetupActive && !setup.state.active) Rebuild();
    for (unsigned i = 0; i < next.size(); ++i) {
        next[i].packet = snapshots[i].packet + (next[i].id != snapshots[i].id || next[i].fields != snapshots[i].fields);
        snapshots[i] = next[i];
    }
    ApplyRumble();
    LogLocked();
}

DWORD WINAPI GetState(DWORD slot, XINPUT_STATE* out) {
    std::shared_lock lock(mutex);
    if (!active) {
        lock.unlock();
        return reinterpret_cast<decltype(&XInputGetState)>(originalState.load())(slot, out);
    }
    if (!out) return ERROR_BAD_ARGUMENTS;
    if (slot >= snapshots.size() || !snapshots[slot].id) return ERROR_DEVICE_NOT_CONNECTED;
    const auto& state = snapshots[slot];
    const auto& f = state.fields;
    *out = {state.packet, {f.buttons, f.leftTrigger, f.rightTrigger, f.leftX, f.leftY, f.rightX, f.rightY}};
    return ERROR_SUCCESS;
}

DWORD WINAPI GetCapabilities(DWORD slot, DWORD flags, XINPUT_CAPABILITIES* out) {
    std::shared_lock lock(mutex);
    if (!active) {
        lock.unlock();
        return reinterpret_cast<decltype(&XInputGetCapabilities)>(originalCapabilities.load())(slot, flags, out);
    }
    if (!out) return ERROR_BAD_ARGUMENTS;
    if (slot >= snapshots.size() || !snapshots[slot].id) return ERROR_DEVICE_NOT_CONNECTED;
    *out = {};
    out->Type = XINPUT_DEVTYPE_GAMEPAD; out->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    out->Gamepad = {0xF3FF, 255, 255, 32767, 32767, 32767, 32767};
    out->Vibration = {65535, 65535};
    return ERROR_SUCCESS;
}

DWORD WINAPI SetState(DWORD slot, XINPUT_VIBRATION* vibration) {
    std::unique_lock lock(mutex);
    if (!active) {
        lock.unlock();
        return reinterpret_cast<decltype(&XInputSetState)>(originalRumble.load())(slot, vibration);
    }
    if (!vibration) return ERROR_BAD_ARGUMENTS;
    if (slot >= snapshots.size() || !snapshots[slot].id || !handles.count(snapshots[slot].id)) return ERROR_DEVICE_NOT_CONNECTED;
    auto& request = rumble[snapshots[slot].id];
    request.left = vibration->wLeftMotorSpeed; request.right = vibration->wRightMotorSpeed; request.pending = true;
    // Import callers can run on another thread. Report the last hardware
    // result; the game-thread poll applies the newest requested motors.
    return request.result;
}

DWORD WINAPI GetBattery(DWORD slot, BYTE deviceType, XINPUT_BATTERY_INFORMATION* out) {
    std::shared_lock lock(mutex);
    if (!active) {
        lock.unlock();
        return reinterpret_cast<decltype(&XInputGetBatteryInformation)>(originalBattery.load())(slot, deviceType, out);
    }
    if (!out) return ERROR_BAD_ARGUMENTS;
    if (slot >= snapshots.size() || !snapshots[slot].id || deviceType != BATTERY_DEVTYPE_GAMEPAD)
        return ERROR_DEVICE_NOT_CONNECTED;
    *out = {BATTERY_TYPE_WIRED, BATTERY_LEVEL_FULL};
    return ERROR_SUCCESS;
}

void CloseLocked() {
    GamepadLayouts::Shutdown();
    for (const auto& [id, handle] : handles) SDL_CloseGamepad(handle);
    handles.clear(); rumble.clear();
    for (const auto& [id, raw] : rawHandles) SDL_CloseJoystick(raw.handle);
    rawHandles.clear(); frames.clear();
    DirectInputFilter::LendToSetup({});
    inventory = {}; snapshots = {};
    SDL_Quit();
    active = false;
}
}

void Install() {
    // DirectInput's window-aware device init at 0x718CE0 can precede Main::Initialize.
    // Arm every import here; the providers preserve native input until Start succeeds.
    requested = imports::PatchImports(GetModuleHandleW(nullptr), {
        {"XINPUT9_1_0.dll", "XInputGetState", reinterpret_cast<void*>(&GetState), originalState},
        {"XINPUT9_1_0.dll", "XInputGetCapabilities", reinterpret_cast<void*>(&GetCapabilities), originalCapabilities},
        {"XINPUT9_1_0.dll", "XInputSetState", reinterpret_cast<void*>(&SetState), originalRumble},
        {"XINPUT1_3.dll", WORD(7), reinterpret_cast<void*>(&GetBattery), originalBattery},
        {"DINPUT8.dll", "DirectInput8Create", reinterpret_cast<void*>(&DirectInputFilter::Create), DirectInputFilter::originalCreate}});
}

void Start() {
    std::unique_lock lock(mutex);
    if (started) return;
    started = true;
    GamepadLayouts::Load();
    if (!requested) { GamepadLayouts::Started({}, false); startupNote = "Input: controller layer off (game imports could not be patched)"; return; }
    OverlayPrefs::Data prefs;
    OverlayPrefs::Load(prefs);
    layerSetting = prefs.controllerLayer;
    if (!layerSetting) { GamepadLayouts::Started({}, false); startupNote = "Input: the game reads controllers itself (setting)"; return; }
    preferred = prefs.deviceIdentity;
    // Only GAMEPAD is initialized, so SDL owns no video or game window. Keep
    // SDL's default backend deduplication; disabling it can expose Xbox pads twice.
    for (const char* hint : {SDL_HINT_JOYSTICK_THREAD, SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
        SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_HIDAPI_PS4, SDL_HINT_JOYSTICK_HIDAPI_PS5,
        SDL_HINT_JOYSTICK_HIDAPI_SWITCH, SDL_HINT_JOYSTICK_HIDAPI_STEAM, SDL_HINT_JOYSTICK_HIDAPI_XBOX,
        SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS}) SDL_SetHint(hint, "1");
    // The caller holds this thread in the multithreaded apartment (fMain::Initialize).
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        GamepadLayouts::Started({}, false);
        startupWarning = true;
        startupNote = "Input: SDL failed to start (" + Text(SDL_GetError()) + "); the game reads controllers itself";
        SDL_Quit();
        return;
    }
    nlohmann::json values; std::string error;
    netplay::SettingsStore store(netplay::SettingsStore::DefaultDirectory());
    if (store.LoadLauncher(values, error))
        for (const auto& [guid, mapping] : netplay::ReadControllerMappings(values)) {
            const bool sdlMaps = !MappingForGuid(guid).empty();
            if (SDL_AddGamepadMapping(mapping.c_str()) >= 0) {
                savedEmberMappingGuids.insert(guid);
                if (sdlMaps) sdlMappedGuids.insert(guid);
            } else spdlog::warn("Input: saved setup for guid {} rejected (SDL: {})", guid, SDL_GetError());
        }
    UpdateLocked(true);
    active = true;
    GamepadLayouts::Started(inventory, true);
    const int version = SDL_GetVersion();
    startupNote = fmt::format("Input: SDL {}.{}.{} started, hidapi {}, background events on, {} saved mappings loaded",
        SDL_VERSIONNUM_MAJOR(version), SDL_VERSIONNUM_MINOR(version), SDL_VERSIONNUM_MICRO(version),
        SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, true) ? "on" : "off", savedEmberMappingGuids.size());
}

void Update() {
    std::unique_lock lock(mutex);
    if (active) UpdateLocked(false);
}

void Shutdown() {
    std::unique_lock lock(mutex);
    if (active) CloseLocked();
}

void LogInventory() {
    std::unique_lock lock(mutex);
    logReady = true;
    GamepadLayouts::FlushLogs();
    LogLocked();
}

std::optional<Identity> IdentityForSlot(int slot) {
    std::shared_lock lock(mutex);
    if (!active || slot < 0 || slot >= static_cast<int>(snapshots.size())) return {};
    for (const auto& pad : inventory.pads) if (pad.slot == slot) return pad.identity;
    return {};
}

PromptBrand BrandForDevice(int deviceType, int slot) {
    std::shared_lock lock(mutex);
    if (active && deviceType == input::PadXInput && slot >= 0 && slot < static_cast<int>(snapshots.size()))
        for (const auto& pad : inventory.pads) if (pad.slot == slot) return GameplayBrand(deviceType, &pad);
    return GameplayBrand(deviceType);
}

std::vector<Pad> ReadPads() {
    std::shared_lock lock(mutex);
    return inventory.pads;
}

LayerFrame ReadFrame() {
    std::shared_lock lock(mutex);
    return {active ? Layer::Active : layerSetting ? Layer::Failed : Layer::GameReads, layerSetting, frames, setup, GamepadLayouts::Stored()};
}

void SetLayouts(std::vector<StoredLayout> layouts) {
    std::unique_lock lock(mutex);
    GamepadLayouts::Set(std::move(layouts));
}
void LendGameTables(GameTableUse use) {
    std::unique_lock lock(mutex);
    if (active) GamepadLayouts::LendGameTables(use);
}
void ReturnGameTables(GameTableUse use) {
    std::unique_lock lock(mutex);
    if (active) GamepadLayouts::ReturnGameTables(use);
}

void SetupPrompt(const Identity& identity) {
    spdlog::info("Input: setup prompt for '{}' guid {}", identity.name, identity.guid);
}

void BeginSetup(const Identity& identity, unsigned padId) {
    std::unique_lock lock(mutex);
    if (!active || setup.state.active) return;
    std::optional<SetupAxisEntry> entry;
    for (const auto& pad : inventory.pads) if (Same(pad.identity, identity)) {
        const auto handle = handles.find(pad.id);
        if (handle == handles.end() || !SDL_GetGamepadButton(handle->second, SDL_GAMEPAD_BUTTON_SOUTH)) break;
        // A failed read finds no axis behind the entry press.
        for (const auto& binding : ReadBindings(handle->second).value_or(std::vector<SdlBinding>{}))
            if (binding.output == SdlTarget("a") && binding.input.type == SdlBindType::Axis) {
                entry = SetupAxisEntry{binding.input.index, binding.input.min}; break;
            }
        break;
    }
    setupModel.Start(GetTickCount64(), entry); setup.padId = padId; setup.identity = identity; setup.state = setupModel.State();
    setup.outcome = SetupEnd::None; setupRestLogged = false; setupCommands.clear();
    UpdateLocked(true);
}
void ControlSetup(SetupCommand command, int step) {
    std::unique_lock lock(mutex);
    if (setup.state.active) setupCommands.emplace_back(command, step);
}
std::optional<std::string> SaveSetup() {
    std::unique_lock lock(mutex);
    if (!active || !setup.state.active) return {};
    setupCommands.emplace_back(SetupCommand::Save, 0);
    UpdateLocked(false);
    if (!active || !setup.state.active || setup.state.mode != SetupMode::Summary || !setup.state.CanSave() || setup.state.feedback == SetupFeedback::Reconnect) return {};
    const auto guid = setup.identity.guid;
    // The merge starts from SDL's snapshot of the pad, its bindings, face style
    // and type; its mapping text only supplies the other fields that bind nothing.
    const auto pad = std::find_if(inventory.pads.begin(), inventory.pads.end(), [](const Pad& candidate) { return Same(candidate.identity, setup.identity); });
    const auto handle = pad == inventory.pads.end() ? handles.end() : handles.find(pad->id);
    const auto snapshot = pad == inventory.pads.end() ? std::nullopt : ReadSnapshot(pad->id, handle == handles.end() ? nullptr : handle->second);
    const auto text = MappingForGuid(guid);
    const auto mapping = SetupMapping(setup.state, snapshot, guid, setup.identity.name, text);
    std::string failure;
    if (!snapshot) failure = fmt::format("the pad's bindings could not be read (SDL: {})", SDL_GetError());
    else if (!mapping) failure = "SDL would take its d-pad for a legacy HIDAPI one and move its buttons";
    else if (SDL_AddGamepadMapping(mapping->c_str()) < 0) failure = fmt::format("SDL rejected {} ({})", *mapping, SDL_GetError());
    if (!failure.empty()) {
        spdlog::error("Input: setup not saved for guid {}: {}", guid, failure);
        setup.outcome = SetupEnd::SaveFailed; ++setup.revision; return {};
    }
    if (!savedEmberMappingGuids.count(guid) && !text.empty()) sdlMappedGuids.insert(guid);
    savedEmberMappingGuids.insert(guid);
    setup.state.active = false; setup.outcome = SetupEnd::Saved; ++setup.revision;
    spdlog::info("Input: setup saved for guid {}: {}", guid, *mapping);
    UpdateLocked(true);
    return mapping;
}
} }
