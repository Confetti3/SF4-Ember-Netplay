#pragma once
#include "GamepadBrand.hxx"
#include "XInputMapping.hxx"
#include "PadSetup.hxx"
#include "ButtonLayoutTypes.hxx"
#include "MenuButtons.hxx"
#include <string>
#include <vector>

namespace sf4e { namespace gamepads {
enum class Layer { Active, GameReads, Failed };
enum class RowState { Recognised, Steam, Unmapped, NotInUse, Absent, GameRead };
struct SetupSnapshot {
    unsigned padId = 0;
    Identity identity;
    SetupState state;
    std::uint64_t revision = 0;
    SetupEnd outcome = SetupEnd::None;
};
inline std::vector<std::string> HeldGlyphs(const State& state) {
    std::vector<std::string> held;
    const struct { const char* id; Button button; bool axis; } inputs[] = {
        {"up", Button::Up, state.leftY < -16384}, {"down", Button::Down, state.leftY > 16383},
        {"left", Button::Left, state.leftX < -16384}, {"right", Button::Right, state.leftX > 16383},
        {"A", Button::South, false}, {"B", Button::East, false}, {"X", Button::West, false}, {"Y", Button::North, false},
        {"LB", Button::LeftShoulder, false}, {"RB", Button::RightShoulder, false},
        {"LS", Button::LeftStick, false}, {"RS", Button::RightStick, false},
        {"View", Button::Back, false}, {"Start", Button::Start, false}
    };
    for (const auto& input : inputs)
        if (input.axis || state.buttons[static_cast<unsigned>(input.button)]) held.emplace_back(input.id);
    for (unsigned i = 0; i < ExtraCount; ++i) if (state.extras[i]) held.emplace_back(ExtraIds[i]);
    if (state.leftTrigger > 16383) held.emplace_back("LT");
    if (state.rightTrigger > 16383) held.emplace_back("RT");
    return held;
}
inline const char* ControllerPicture(const Pad& pad) {
    if (SetUpOnlyByEmber(pad) || pad.arcadeStick) return "generic_joystick_red";
    switch (pad.type) {
    case Type::Xbox360: return "controller_xbox360";
    case Type::XboxOne: return "controller_xboxseries";
    case Type::PS3: return "controller_playstation3";
    case Type::PS4: return "controller_playstation4";
    case Type::PS5: return "controller_playstation5";
    case Type::SwitchPro: return "controller_switch_pro";
    case Type::JoyConLeft: case Type::JoyConRight: case Type::JoyConPair: return "controller_switch";
    default: return SteamVirtual(pad) ? "controller_steam" : "controller_generic";
    }
}
// One copied shape for the list, capture pictures and per-frame input test.
// id is a stable session row number, independent of SDL instance and game slot.
struct PadView {
    unsigned id = 0;
    unsigned connectionOrder = 0;
    Identity identity;
    std::string name, picture;
    PromptBrand brand;
    Type artVariant = Type::Unknown;
    FightNames buttonNames;
    int slot = -1, directInputIndex = -1;
    RowState state = RowState::Recognised;
    bool gameplay = false, customMapping = false, pressed = false;
    std::vector<std::string> held;
    std::vector<RawInput> raw;
    bool rawAtRest = true;
    // What SDL binds for a pad with an Ember setup; Stick names read it.
    std::vector<SdlBinding> bindings;
    bool numberedHats = false;
    ListedButtons has = AllLayoutButtons;
    ButtonLayout table = DefaultLayout;
    std::optional<ButtonLayout> layout;
    // The stored menu binding, and the buttons that confirm and go back for
    // this pad with it (Pad::MenuButtons; A and B while it has no game slot).
    MenuBinding menu;
    input::MenuButtons menuButtons;
    // The extra buttons the pad reports and what each copies.
    ListedExtras hasExtras{};
    ExtraCopies extras = NoExtraCopies;
    // The stored buttons that start a Training recording and play a slot.
    TrainingBinding training;
};
struct PadFrame {
    Pad pad;
    State state;
    std::vector<RawInput> raw;
    bool pressed = false;
    bool rawAtRest = true;
    std::vector<SdlBinding> bindings;
    bool numberedHats = false;
    ListedButtons has = AllLayoutButtons;
    ButtonLayout table = DefaultLayout;
    std::optional<ButtonLayout> layout;
    MenuBinding menu;
    ListedExtras hasExtras{};
    ExtraCopies extras = NoExtraCopies;
    TrainingBinding training;
};
struct NativeController {
    int index;
    std::string instanceName, productName, path;
};
inline void OrderPadRows(std::vector<PadView>& pads) {
    const auto order = [](const PadView& pad) {
        if (pad.state == RowState::Recognised || pad.state == RowState::Steam) return std::make_pair(0, pad.slot);
        if (pad.state == RowState::Unmapped || pad.state == RowState::GameRead) return std::make_pair(1, pad.directInputIndex);
        return std::make_pair(pad.state == RowState::NotInUse ? 2 : 3,
            static_cast<int>(pad.state == RowState::NotInUse ? pad.connectionOrder : pad.id));
    };
    std::stable_sort(pads.begin(), pads.end(), [&](const PadView& a, const PadView& b) { return order(a) < order(b); });
}
inline bool LayerPending(bool savedControllerLayer, bool runningControllerLayer) { return savedControllerLayer != runningControllerLayer; }
} }
