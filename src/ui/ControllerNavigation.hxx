#pragma once
#include "MenuNavigation.hxx"
#include "../common/PadKind.hxx"
#include "../common/MenuButtons.hxx"
#include <cstdint>

namespace sf4e { namespace ui {

// Copied on the game thread. Device identity is independent of lobby position.
struct ControllerSample {
    // Every button but Menu carries MenuInput's value, because the overlay
    // hands a sample's buttons to the menu unchanged; the asserts below hold
    // the two enums together.
    // Menu (Start) is the game thread's and never reaches a menu.
    enum Button : std::uint32_t { Up = 1, Down = 2, Left = 4, Right = 8, Confirm = 16, Back = 32, Menu = 64,
        Fighter = 128, Options = 256, Chat = 512, PrevTab = 2048, NextTab = 4096, Describe = 8192 };
    int deviceType = -1, deviceIndex = -1;
    bool connected = false;
    std::uint32_t buttons = 0;
    // The device's confirm and back buttons and their glyphs (Pad::MenuButtons).
    input::MenuButtons menu;
};
static_assert(ControllerSample::Up == MenuInput::Up && ControllerSample::Down == MenuInput::Down &&
    ControllerSample::Left == MenuInput::Left && ControllerSample::Right == MenuInput::Right &&
    ControllerSample::Confirm == MenuInput::Select && ControllerSample::Back == MenuInput::Back &&
    ControllerSample::Fighter == MenuInput::Fighter && ControllerSample::Options == MenuInput::Options &&
    ControllerSample::Chat == MenuInput::Chat && ControllerSample::PrevTab == MenuInput::PrevTab &&
    ControllerSample::NextTab == MenuInput::NextTab && ControllerSample::Describe == MenuInput::Describe && ControllerSample::Menu == MenuInput::Start, "ControllerSample and MenuInput must number their buttons alike");
static_assert((ControllerSample::Menu & (MenuInput::Up | MenuInput::Down | MenuInput::Left | MenuInput::Right |
    MenuInput::Select | MenuInput::Back | MenuInput::Fighter | MenuInput::Options | MenuInput::Chat)) == 0,
    "Start must not collide with a menu button");

// Directions use native fight-map bits, not the battle command buffer. Select
// and Back are the device's physical menu buttons (Pad::MenuButtons): the
// player's binding, else the ones the game's own menus use. On an Xbox pad X,
// Y and View are shortcuts unless one of them confirms or goes back; Start
// (the game's Start function on DirectInput) opens Ember. RT, the fourth
// button of the bottom row on an Xbox-layout stick too, is the accessibility
// button (MenuInput::Describe).
inline std::uint32_t ControllerButtons(std::uint32_t held,int deviceType=-1,std::uint32_t physical=0,const input::MenuButtons& menu={}) {
    std::uint32_t result = 0;
    if (held & 0x1) result |= ControllerSample::Up;
    if (held & 0x2) result |= ControllerSample::Down;
    if (held & 0x4) result |= ControllerSample::Left;
    if (held & 0x8) result |= ControllerSample::Right;
    if (physical & menu.confirm) result |= ControllerSample::Confirm;
    if (physical & menu.back) result |= ControllerSample::Back;
    if(deviceType==input::PadXInput){
        const auto shortcuts=physical&~(menu.confirm|menu.back);
        if(shortcuts&input::xinput::X)result|=ControllerSample::Fighter;
        if(shortcuts&input::xinput::Y)result|=ControllerSample::Options;
        if(shortcuts&input::xinput::View)result|=ControllerSample::Chat;
        if(shortcuts&input::xinput::LB)result|=ControllerSample::PrevTab;
        if(shortcuts&input::xinput::RB)result|=ControllerSample::NextTab;
    }
    if(input::StartHeld(deviceType,held,physical))result|=ControllerSample::Menu;
    if (physical & ~(menu.confirm | menu.back) & input::xinput::RT) result |= ControllerSample::Describe;
    return result;
}

class ControllerNavigation {
public:
    // Call before ImGui::NewFrame, after the platform backend. Only this adapter
    // supplies gamepad keys; physical keyboard input remains independently owned.
    // While not enabled (a device being assigned) nothing is read. A button
    // held when the menu shows or is enabled again waits for its release
    // (PressGate); the others count at once.
    void Update(const ControllerSample& sample, bool atMainMenu, bool visible, bool enabled);
    void Reset();
    bool MenuGuard() const { return menuGuard_; }
    bool BackRequested() const { return backRequested_; }
    bool FocusRequested() const { return focusRequested_; }
    bool Available() const { return available_; }
    std::uint32_t Buttons() const { return output_; }
    bool Unavailable() const { return !available_ && (deviceType_ == input::PadXInput || deviceType_ == input::PadDirectInput); }
private:
    int deviceType_ = -1, deviceIndex_ = -1;
    bool wasVisible_ = false, draining_ = false;
    bool menuGuard_ = false, backRequested_ = false, focusRequested_ = false, available_ = false;
    bool widgetBack_ = false;
    PressGate gate_;
    std::uint32_t output_ = 0;
};

} }
