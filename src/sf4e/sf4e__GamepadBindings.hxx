#pragma once
#include "../common/GamepadSlots.hxx"
#include "../common/SdlMapping.hxx"
#include "../common/SdlPadSource.hxx"
#include <SDL3/SDL.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace gamepads {
static_assert(static_cast<int>(SdlBindType::Button) == SDL_GAMEPAD_BINDTYPE_BUTTON && static_cast<int>(SdlBindType::Axis) == SDL_GAMEPAD_BINDTYPE_AXIS &&
    static_cast<int>(SdlBindType::Hat) == SDL_GAMEPAD_BINDTYPE_HAT && SdlLeftTrigger == SDL_GAMEPAD_AXIS_LEFT_TRIGGER &&
    SdlDpadUp == SDL_GAMEPAD_BUTTON_DPAD_UP && SdlButtonNames.size() == SDL_GAMEPAD_BUTTON_COUNT && SdlAxisNames.size() == SDL_GAMEPAD_AXIS_COUNT,
    "SDL's binding model moved");
// What SDL binds for an open gamepad, in SDL's order, with label hints and the
// HIDAPI fixup already applied. None when SDL cannot copy them, which is not a
// pad that binds nothing.
inline std::optional<std::vector<SdlBinding>> ReadBindings(SDL_Gamepad* gamepad) {
    int count = 0;
    SDL_GamepadBinding** bindings = SDL_GetGamepadBindings(gamepad, &count);
    if (!bindings) return {};
    std::vector<SdlBinding> read;
    for (int i = 0; i < count; ++i) {
        const auto& sdl = *bindings[i];
        SdlBinding binding;
        binding.input.type = static_cast<SdlBindType>(sdl.input_type);
        if (sdl.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON) binding.input.index = sdl.input.button;
        else if (sdl.input_type == SDL_GAMEPAD_BINDTYPE_HAT) { binding.input.index = sdl.input.hat.hat; binding.input.hatMask = sdl.input.hat.hat_mask; }
        else binding.input = {SdlBindType::Axis, sdl.input.axis.axis, sdl.input.axis.axis_min, sdl.input.axis.axis_max};
        if (sdl.output_type == SDL_GAMEPAD_BINDTYPE_BUTTON) binding.output = {SdlBindType::Button, sdl.output.button};
        else binding.output = {SdlBindType::Axis, sdl.output.axis.axis, sdl.output.axis.axis_min, sdl.output.axis.axis_max};
        read.push_back(binding);
    }
    SDL_free(bindings);
    return read;
}
// The face style SDL labels by, told apart by its south and east labels.
inline SdlFace FaceFromLabels(SDL_GamepadButtonLabel south, SDL_GamepadButtonLabel east) {
    if (south == SDL_GAMEPAD_BUTTON_LABEL_CROSS) return SdlFace::Sony;
    if (south == SDL_GAMEPAD_BUTTON_LABEL_B) return SdlFace::Bayx;
    return east == SDL_GAMEPAD_BUTTON_LABEL_X ? SdlFace::Axby : SdlFace::Abxy;
}
// SDL's name for the type it gives an open gamepad, its mapping's or the
// device's own; empty when that is the type SDL finds for the device itself
// (SDL_UpdateGamepadType's fallback), which a mapping need not name.
inline std::string TypeField(SDL_Gamepad* gamepad) {
    const auto type = SDL_GetGamepadType(gamepad);
    if (type == SDL_GetRealGamepadTypeForID(SDL_GetGamepadID(gamepad))) return {};
    const char* name = SDL_GetGamepadStringForType(type);
    return name ? name : "";
}
// The one way Ember knows a pad: what SDL makes of it by id, from its open
// gamepad or one opened for the read. A pad SDL has no mapping for binds
// nothing, takes the face style of its type and keeps the device's own type.
// None when SDL cannot open a mapped pad or copy its bindings.
inline std::optional<SdlSnapshot> ReadSnapshot(SDL_JoystickID id, SDL_Gamepad* open) {
    if (!open && !SDL_IsGamepad(id)) {
        const auto type = SDL_GetGamepadTypeForID(id);
        return SdlSnapshot{{}, FaceFromLabels(SDL_GetGamepadButtonLabelForType(type, SDL_GAMEPAD_BUTTON_SOUTH),
            SDL_GetGamepadButtonLabelForType(type, SDL_GAMEPAD_BUTTON_EAST))};
    }
    SDL_Gamepad* gamepad = open ? open : SDL_OpenGamepad(id);
    if (!gamepad) return {};
    std::optional<SdlSnapshot> read;
    if (auto bindings = ReadBindings(gamepad))
        read = SdlSnapshot{std::move(*bindings), FaceFromLabels(SDL_GetGamepadButtonLabel(gamepad, SDL_GAMEPAD_BUTTON_SOUTH),
            SDL_GetGamepadButtonLabel(gamepad, SDL_GAMEPAD_BUTTON_EAST)), TypeField(gamepad)};
    if (!open) SDL_CloseGamepad(gamepad);
    return read;
}
} }
