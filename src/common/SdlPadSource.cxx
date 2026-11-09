#include "SdlPadSource.hxx"
#include <algorithm>

namespace sf4e { namespace gamepads {

std::string SdlVersion() {
    const int version = SDL_GetVersion();
    return std::to_string(SDL_VERSIONNUM_MAJOR(version)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(version)) + "." +
        std::to_string(SDL_VERSIONNUM_MICRO(version));
}

State ReadState(SDL_Gamepad* gamepad) {
    constexpr std::array<SDL_GamepadButton, static_cast<unsigned>(Button::Count)> buttons = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT
    };
    State state;
    for (unsigned i = 0; i < buttons.size(); ++i) state.buttons[i] = SDL_GetGamepadButton(gamepad, buttons[i]);
    for (unsigned i = 0; i < ExtraCount; ++i) state.extras[i] = SDL_GetGamepadButton(gamepad, SdlExtraButtons[i]);
    state.leftX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    state.leftY = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    state.rightX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    state.rightY = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
    state.leftTrigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    state.rightTrigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    return state;
}

std::optional<std::string> SdlPadSource::Start(SdlBackends backends) {
    // Keep SDL's default backend deduplication; disabling it can expose Xbox pads twice.
    for (const char* hint : {SDL_HINT_JOYSTICK_THREAD, SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
        SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_HIDAPI_PS4, SDL_HINT_JOYSTICK_HIDAPI_PS5,
        SDL_HINT_JOYSTICK_HIDAPI_SWITCH, SDL_HINT_JOYSTICK_HIDAPI_STEAM,
        SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS}) SDL_SetHint(hint, "1");
    if (backends == SdlBackends::Every) SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    else for (const char* hint : {SDL_HINT_XINPUT_ENABLED, SDL_HINT_JOYSTICK_HIDAPI_XBOX, SDL_HINT_JOYSTICK_RAWINPUT,
        SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT, SDL_HINT_JOYSTICK_GAMEINPUT_RAW}) SDL_SetHint(hint, "0");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        const auto error = SdlText(SDL_GetError());
        SDL_Quit();
        return error;
    }
    started = true;
    return std::nullopt;
}

void SdlPadSource::Stop() {
    if (!started) return;
    for (const auto& held : handles) SDL_CloseGamepad(held.second);
    handles.clear();
    SDL_Quit();
    started = false;
}

bool SdlPadSource::Pump() {
    SDL_UpdateGamepads();
    SDL_PumpEvents();
    bool changed = false;
    SDL_Event events[32];
    int count;
    while ((count = SDL_PeepEvents(events, 32, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST)) > 0)
        for (int i = 0; i < count; ++i) switch (events[i].type) {
        case SDL_EVENT_JOYSTICK_ADDED: case SDL_EVENT_JOYSTICK_REMOVED:
        case SDL_EVENT_GAMEPAD_ADDED: case SDL_EVENT_GAMEPAD_REMOVED: case SDL_EVENT_GAMEPAD_REMAPPED:
            changed = true; break;
        default: break;
        }
    return changed;
}

std::optional<std::vector<Pad>> SdlPadSource::List() const {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) return std::nullopt;
    std::vector<Pad> pads;
    for (int i = 0; i < count; ++i) {
        Pad pad;
        pad.id = ids[i];
        pad.vid = SDL_GetJoystickVendorForID(pad.id); pad.pid = SDL_GetJoystickProductForID(pad.id);
        pad.type = PadType(pad.id);
        pad.arcadeStick = SDL_GetJoystickTypeForID(pad.id) == SDL_JOYSTICK_TYPE_ARCADE_STICK;
        char guid[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(pad.id), guid, sizeof(guid));
        pad.identity = {guid, "", SdlText(SDL_GetJoystickPathForID(pad.id)), SdlText(SDL_GetJoystickNameForID(pad.id))};
        pad.mapped = pad.sdlMapping = SDL_IsGamepad(pad.id);
        pads.push_back(std::move(pad));
    }
    SDL_free(ids);
    return pads;
}

std::vector<OpenFailure> SdlPadSource::Hold(std::vector<Pad>& pads) {
    const auto wanted = [](const Pad& pad) { return pad.status == Status::NotInUse; };
    for (auto it = handles.begin(); it != handles.end();) {
        const bool keep = std::any_of(pads.begin(), pads.end(), [&](const Pad& pad) { return pad.id == it->first && wanted(pad); });
        if (keep) { ++it; continue; }
        SDL_CloseGamepad(it->second);
        it = handles.erase(it);
    }
    std::vector<OpenFailure> failures;
    for (auto& pad : pads) if (wanted(pad)) {
        auto handle = handles.find(pad.id);
        if (handle == handles.end()) {
            if (auto* opened = SDL_OpenGamepad(pad.id)) handle = handles.emplace(pad.id, opened).first;
            else failures.push_back({pad.identity.name, SdlText(SDL_GetError())});
        }
        pad.available = handle != handles.end();
        if (pad.available) pad.identity.serial = SdlText(SDL_GetGamepadSerial(handle->second));
    }
    return failures;
}

SDL_Gamepad* SdlPadSource::Handle(Id id) const {
    const auto handle = handles.find(id);
    return handle == handles.end() ? nullptr : handle->second;
}

} }
