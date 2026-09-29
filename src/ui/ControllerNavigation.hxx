#pragma once
#include "MenuNavigation.hxx"
#include "../common/PadKind.hxx"
#include <cstdint>

namespace sf4e { namespace ui {

// Copied on the game thread. Device identity is independent of lobby position.
struct ControllerSample {
    // Every button but Menu carries MenuInput's value, because the overlay
    // hands a sample's buttons to the menu unchanged; the asserts below hold
    // the two enums together. Menu (Start) opens Ember and never reaches a menu.
    enum Button : std::uint32_t { Up = 1, Down = 2, Left = 4, Right = 8, Confirm = 16, Back = 32, Menu = 64,
        Fighter = 128, Options = 256, Chat = 512 };
    int deviceType = -1, deviceIndex = -1;
    bool connected = false;
    std::uint32_t buttons = 0;
    std::uint32_t selectPhysical = 0, backPhysical = 0;
};
static_assert(ControllerSample::Up == MenuInput::Up && ControllerSample::Down == MenuInput::Down &&
    ControllerSample::Left == MenuInput::Left && ControllerSample::Right == MenuInput::Right &&
    ControllerSample::Confirm == MenuInput::Select && ControllerSample::Back == MenuInput::Back &&
    ControllerSample::Fighter == MenuInput::Fighter && ControllerSample::Options == MenuInput::Options &&
    ControllerSample::Chat == MenuInput::Chat, "ControllerSample and MenuInput must number their buttons alike");
static_assert((ControllerSample::Menu & (MenuInput::Up | MenuInput::Down | MenuInput::Left | MenuInput::Right |
    MenuInput::Select | MenuInput::Back | MenuInput::Fighter | MenuInput::Options | MenuInput::Chat)) == 0,
    "Start must not collide with a menu button");

// Directions use native fight-map bits, not the battle command buffer. Xbox
// menu actions use copied physical A/B, and X, Y and View are shortcuts;
// unidentified DirectInput retains LP/LK and has none.
inline std::uint32_t ControllerButtons(std::uint32_t held,int deviceType=-1,std::uint32_t physical=0) {
    std::uint32_t result = 0;
    if (held & 0x1) result |= ControllerSample::Up;
    if (held & 0x2) result |= ControllerSample::Down;
    if (held & 0x4) result |= ControllerSample::Left;
    if (held & 0x8) result |= ControllerSample::Right;
    if(deviceType==input::PadXInput){
        // Menu actions use physical A/B, independently of fighting bindings.
        if(physical&input::xinput::A)result|=ControllerSample::Confirm;
        if(physical&input::xinput::B)result|=ControllerSample::Back;
        if(physical&input::xinput::Start)result|=ControllerSample::Menu;
        if(physical&input::xinput::X)result|=ControllerSample::Fighter;
        if(physical&input::xinput::Y)result|=ControllerSample::Options;
        if(physical&input::xinput::View)result|=ControllerSample::Chat;
    }else{
        if (held & (0x10 | 0x1000)) result |= ControllerSample::Confirm;
        if (held & (0x40 | 0x2000)) result |= ControllerSample::Back;
        if (held & 0x1000) result |= ControllerSample::Menu;
    }
    return result;
}

class ControllerNavigation {
public:
    // Call before ImGui::NewFrame, after the platform backend. Only this adapter
    // supplies gamepad keys; physical keyboard input remains independently owned.
    void Update(const ControllerSample& sample, bool atMainMenu, bool visible, bool focused);
    void Reset();
    bool MenuGuard() const { return menuGuard_; }
    bool BackRequested() const { return backRequested_; }
    bool OpenRequested() const { return openRequested_; }
    bool FocusRequested() const { return focusRequested_; }
    bool Available() const { return available_; }
    std::uint32_t Buttons() const { return output_; }
    bool Unavailable() const { return !available_ && (deviceType_ == input::PadXInput || deviceType_ == input::PadDirectInput); }
private:
    int deviceType_ = -1, deviceIndex_ = -1;
    bool armed_ = false, wasVisible_ = false, draining_ = false;
    bool menuGuard_ = false, backRequested_ = false, focusRequested_ = false, available_ = false;
    bool widgetBack_ = false;
    bool menuArmed_ = false, openRequested_ = false;
    std::uint32_t previous_ = 0;
    std::uint32_t output_ = 0;
};

} }
