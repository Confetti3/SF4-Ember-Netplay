#pragma once
#include "ExtraButtons.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>

namespace sf4e { namespace gamepads {

enum class Button { South, East, West, North, Back, Guide, Start, LeftStick, RightStick,
    LeftShoulder, RightShoulder, Up, Down, Left, Right, Count };

struct State {
    std::array<bool, static_cast<unsigned>(Button::Count)> buttons{};
    std::array<bool, ExtraCount> extras{};
    int leftX = 0, leftY = 0, rightX = 0, rightY = 0, leftTrigger = 0, rightTrigger = 0;
};

struct XInputFields {
    std::uint16_t buttons = 0;
    std::uint8_t leftTrigger = 0, rightTrigger = 0;
    std::int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    bool operator==(const XInputFields& other) const {
        return std::tie(buttons, leftTrigger, rightTrigger, leftX, leftY, rightX, rightY) ==
            std::tie(other.buttons, other.leftTrigger, other.rightTrigger, other.leftX, other.leftY, other.rightX, other.rightY);
    }
    bool operator!=(const XInputFields& other) const { return !(*this == other); }
};

inline XInputFields MapToXInput(const State& state) {
    // Raw XInput bits, not the game's normalized PadKind.hxx button bits. Guide has no XInput 9.1.0 bit.
    constexpr std::array<std::uint16_t, static_cast<unsigned>(Button::Count)> bits = {
        0x1000, 0x2000, 0x4000, 0x8000, 0x0020, 0, 0x0010, 0x0040, 0x0080,
        0x0100, 0x0200, 0x0001, 0x0002, 0x0004, 0x0008
    };
    const auto axis = [](int value) { return static_cast<std::int16_t>(std::clamp(value, -32768, 32767)); };
    const auto inverted = [&](int value) { return axis(-std::clamp(value, -32768, 32767)); };
    const auto trigger = [](int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 32767) * 255 / 32767); };
    XInputFields result;
    for (unsigned i = 0; i < bits.size(); ++i) if (state.buttons[i]) result.buttons |= bits[i];
    result.leftX = axis(state.leftX); result.leftY = inverted(state.leftY);
    result.rightX = axis(state.rightX); result.rightY = inverted(state.rightY);
    result.leftTrigger = trigger(state.leftTrigger); result.rightTrigger = trigger(state.rightTrigger);
    return result;
}

// The state with each held extra button pressing the layout button it copies,
// as Steam Input binds a touchpad or paddle to an Xbox button.
inline State WithExtraCopies(State state, const ExtraCopies& copies) {
    // The layout buttons in LayoutGlyphs order; LT and RT are axes, pressed fully.
    constexpr int LeftTrigger = 6, RightTrigger = 7;
    constexpr std::array<Button, 10> buttons = {Button::South, Button::East, Button::West, Button::North,
        Button::LeftShoulder, Button::RightShoulder, Button::Count, Button::Count, Button::LeftStick, Button::RightStick};
    for (unsigned i = 0; i < ExtraCount; ++i) {
        const int copy = copies[i];
        if (!state.extras[i] || copy < 0 || copy >= static_cast<int>(buttons.size())) continue;
        if (copy == LeftTrigger) state.leftTrigger = 32767;
        else if (copy == RightTrigger) state.rightTrigger = 32767;
        else state.buttons[static_cast<unsigned>(buttons[copy])] = true;
    }
    return state;
}

} }
