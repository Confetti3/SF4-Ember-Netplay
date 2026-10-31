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
    // the two enums together. Menu (Start) opens Ember; once it is shown it
    // reaches the menu as MenuInput::Start, which only the on-screen keyboard reads.
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

// The menu's shortcuts: the MenuInput press each is, the Xbox pad button that
// gives it, the fight function that gives it on a DirectInput stick (0 for
// none), and the keyboard key. A stick's shape cannot be known, so it has only
// the two tab buttons, the ends of its top row, named by their fight notation
// as its Select and Back are (CONTROLLER_LAYER_UX 11.14). Leave is the Delete
// key alone: a pad leaves its seat with Back on its own card.
struct MenuShortcut { unsigned press; std::uint32_t xinput, stick; const char* pad; const char* key; };
constexpr MenuShortcut MenuShortcuts[] = {
    {MenuInput::Fighter, input::xinput::X, 0, "X", "F"}, {MenuInput::Options, input::xinput::Y, 0, "Y", "T"},
    {MenuInput::Chat, input::xinput::View, 0, "View", "C"}, {MenuInput::Leave, 0, 0, nullptr, "Del"},
    {MenuInput::PrevTab, input::xinput::LB, 0x10, "LB", "PgUp"}, {MenuInput::NextTab, input::xinput::RB, 0x400, "RB", "PgDn"}};
// The glyph that names shortcut `press` on a device (input::PadKeyboard,
// PadXInput, PadDirectInput), or null where the device has no such button, as
// on an Xbox pad whose button the player made Select or Back. The hint bar,
// the tab strip and the on-screen keyboard draw only what this names.
inline const char* MenuShortcutGlyph(unsigned press, int deviceType, const input::MenuButtons& menu) {
    for (const auto& s : MenuShortcuts) if (s.press == press) {
        if (deviceType == input::PadKeyboard) return s.key;
        if (deviceType == input::PadXInput) return s.xinput & ~(menu.confirm | menu.back) ? s.pad : nullptr;
        return deviceType == input::PadDirectInput && s.stick ? input::MenuFunctionName(s.stick) : nullptr;
    }
    return nullptr;
}
// Directions use native fight-map bits, not the battle command buffer. Select
// and Back are the device's physical menu buttons (Pad::MenuButtons): the
// player's binding, else the ones the game's own menus use. The shortcuts are
// MenuShortcuts, an Xbox pad's unless its button confirms or goes back. Both
// tab buttons at once (a stick's 3P) step no tab. Start (the game's Start
// function on DirectInput) opens Ember. RT, the fourth button of the bottom
// row on an Xbox-layout stick too, is the accessibility button (Describe).
inline std::uint32_t ControllerButtons(std::uint32_t held,int deviceType=-1,std::uint32_t physical=0,const input::MenuButtons& menu={}) {
    std::uint32_t result = 0;
    if (held & 0x1) result |= ControllerSample::Up;
    if (held & 0x2) result |= ControllerSample::Down;
    if (held & 0x4) result |= ControllerSample::Left;
    if (held & 0x8) result |= ControllerSample::Right;
    if (physical & menu.confirm) result |= ControllerSample::Confirm;
    if (physical & menu.back) result |= ControllerSample::Back;
    const auto shortcuts = physical & ~(menu.confirm | menu.back);
    for (const auto& s : MenuShortcuts)
        if (deviceType == input::PadXInput ? (shortcuts & s.xinput) != 0 : deviceType == input::PadDirectInput && (held & s.stick)) result |= s.press;
    const std::uint32_t tabs = ControllerSample::PrevTab | ControllerSample::NextTab;
    if ((result & tabs) == tabs) result &= ~tabs;
    if (deviceType == input::PadXInput ? (physical & input::xinput::Start) != 0 : (held & 0x1000) != 0) result |= ControllerSample::Menu;
    if (shortcuts & input::xinput::RT) result |= ControllerSample::Describe;
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
    bool OpenRequested() const { return openRequested_; }
    bool FocusRequested() const { return focusRequested_; }
    bool Available() const { return available_; }
    std::uint32_t Buttons() const { return output_; }
    bool Unavailable() const { return !available_ && (deviceType_ == input::PadXInput || deviceType_ == input::PadDirectInput); }
private:
    int deviceType_ = -1, deviceIndex_ = -1;
    bool wasVisible_ = false, draining_ = false;
    bool menuGuard_ = false, backRequested_ = false, focusRequested_ = false, available_ = false;
    bool widgetBack_ = false;
    bool menuArmed_ = false, openRequested_ = false;
    PressGate gate_;
    std::uint32_t output_ = 0;
};

} }
