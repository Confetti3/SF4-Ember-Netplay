#pragma once
// SDL's gamepads for a game's controller layer (SF4 Ember's sf4e__Gamepads,
// SFV Ember's mod/SdlPads): SDL started under the game's backend choice, its
// pads as gamepads::Pad before the layer opens any, the gamepad handles the
// layer holds, a pad's state and the event pump. The layer still decides
// which pad takes which slot (GamepadSlots.hxx), how the game reads it and
// when its motors turn.
//
// One thread at a time: whichever thread the layer reads SDL on.
#include "GamepadSlots.hxx"
#include "XInputMapping.hxx"
#include <SDL3/SDL.h>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace gamepads {

// Which of SDL's joystick backends read pads: the game's choice.
enum class SdlBackends {
    // Every pad, an Xbox pad as well (SDL's Xbox HIDAPI driver is on): SF4
    // Ember's layer answers all four XInput slots itself.
    Every,
    // No XInput device. SDL's XInput, raw input, Windows.Gaming.Input and
    // GameInput backends and its Xbox HIDAPI driver are off; SDL's DirectInput
    // still lists an XInput device, by its IG_ path, for the layer to leave to
    // the game. SFV Ember's layer passes the game's XInput pads through and
    // answers only the slots XInput leaves empty, so these hints are
    // load-bearing: its XInput hooks pass straight to XInput only on the
    // layer's own thread, and SDL reads pads on threads of its own too
    // (its joystick thread, HIDAPI's), so SDL's XInput backend would read the
    // layer's slots back as pads.
    NotXInput,
};

// SDL's text, or empty for none.
inline std::string SdlText(const char* text) { return text ? text : ""; }

// The SDL linked in, as "3.4.16".
std::string SdlVersion();

// The type Ember gives a pad by id, the one SDL reports for it: its mapping's
// type field, or the type SDL finds for the device.
inline Type PadType(Id id) {
    switch (SDL_GetGamepadTypeForID(id)) {
    case SDL_GAMEPAD_TYPE_STANDARD: return Type::Standard;
    case SDL_GAMEPAD_TYPE_XBOX360: return Type::Xbox360;
    case SDL_GAMEPAD_TYPE_XBOXONE: return Type::XboxOne;
    case SDL_GAMEPAD_TYPE_PS3: return Type::PS3;
    case SDL_GAMEPAD_TYPE_PS4: return Type::PS4;
    case SDL_GAMEPAD_TYPE_PS5: return Type::PS5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return Type::SwitchPro;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return Type::JoyConLeft;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return Type::JoyConRight;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return Type::JoyConPair;
    case SDL_GAMEPAD_TYPE_GAMECUBE: return Type::GameCube;
    default: return Type::Unknown;
    }
}

// SDL's buttons for the extra buttons, in Extra order.
constexpr std::array<SDL_GamepadButton, ExtraCount> SdlExtraButtons = {
    SDL_GAMEPAD_BUTTON_TOUCHPAD, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_MISC1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2};

// An open pad's buttons, extra buttons, triggers and sticks as SDL's gamepad
// mapping names them (XInputMapping.hxx's order).
State ReadState(SDL_Gamepad* gamepad);

// A pad SDL would not open, and SDL's reason.
struct OpenFailure {
    std::string name, error;
};

// The owner calls Stop before the source goes: its destructor leaves SDL as
// it is, since a static source is destroyed at DLL detach, under the loader
// lock, where SDL_Quit's wait for SDL's threads would never end.
class SdlPadSource {
public:
    SdlPadSource() = default;
    SdlPadSource(const SdlPadSource&) = delete;
    SdlPadSource& operator=(const SdlPadSource&) = delete;

    // Sets SDL's hints for `backends` and starts its gamepad subsystem alone,
    // so SDL owns no video or game window. SDL's error when it cannot start
    // (SDL is quit again).
    std::optional<std::string> Start(SdlBackends backends);
    // Closes every handle and quits SDL. Nothing when SDL is not started.
    void Stop();

    // Brings SDL's pads up to date and empties its event queue: true when a
    // pad came, went or was remapped, so the layer lists them again.
    bool Pump();
    // SDL's pads as it lists them, before the layer opens any: no serial yet,
    // and `mapped` and `sdlMapping` say whether SDL maps the pad itself (the
    // game may ask more). None when SDL cannot list them (SDL_GetError).
    std::optional<std::vector<Pad>> List() const;
    // Holds a gamepad handle for exactly the pads the layer may slot (NotInUse
    // after Classify): closes any other's, opens the missing ones, and gives
    // each of those pads `available` (it opened) and the serial SDL reads from
    // it. The pads SDL would not open.
    std::vector<OpenFailure> Hold(std::vector<Pad>& pads);
    // The handle held for a pad, or null.
    SDL_Gamepad* Handle(Id id) const;

private:
    std::map<Id, SDL_Gamepad*> handles;
    bool started = false;
};

} }
