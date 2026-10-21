#pragma once
#include "ControllersScreen.hxx"
#include "../common/ButtonLayout.hxx"

namespace sf4e { namespace ui {
struct LayoutFunction { const char* id; const char* key; std::uint32_t mask; };
constexpr std::array<LayoutFunction, 11> LayoutFunctions = {{
    {"lp", "pad_setup.step.lp", 0x10}, {"mp", "pad_setup.step.mp", 0x20}, {"hp", "pad_setup.step.hp", 0x400},
    {"lk", "pad_setup.step.lk", 0x40}, {"mk", "pad_setup.step.mk", 0x80}, {"hk", "pad_setup.step.hk", 0x800},
    {"3p", "pad_setup.step.3p", 0x430}, {"3k", "pad_setup.step.3k", 0x8C0},
    {"throw", "button_layout.throw", 0x50}, {"focus", "button_layout.focus", 0xA0},
    {"red-focus", "button_layout.red_focus", 0xB0}
}};
std::string LayoutName(const std::optional<gamepads::ButtonLayout>& layout);
// The captures of the Menu confirm, Menu back, Training record and Training
// play rows, numbered after LayoutFunctions.
constexpr unsigned MenuConfirmCapture = static_cast<unsigned>(LayoutFunctions.size()), MenuBackCapture = MenuConfirmCapture + 1,
    TrainingRecordCapture = MenuBackCapture + 1, TrainingPlayCapture = TrainingRecordCapture + 1;
// The row a capture belongs to: fn- and the function's id for an assign,
// menu-confirm or menu-back for a menu button, training-record or
// training-play for a Training button, edit-button for a pick.
inline std::string LayoutCaptureRow(const std::optional<unsigned>& function) {
    if (function && *function >= TrainingRecordCapture) return *function == TrainingRecordCapture ? "training-record" : "training-play";
    if (function && *function >= MenuConfirmCapture) return *function == MenuConfirmCapture ? "menu-confirm" : "menu-back";
    return function ? std::string("fn-") + LayoutFunctions[*function].id : std::string("edit-button");
}
// The pad's name for a physical menu button (input::MenuButtons), or its glyph
// when it is none of the ten layout buttons.
std::string MenuButtonName(const gamepads::PadView& pad, std::uint32_t mask, const char* glyph);
std::string AttackLongName(std::uint32_t mask);
gamepads::FightNames LayoutButtonNames(const gamepads::PadView& pad, gamepads::ButtonPrompts prompts);
// The pad's name for an extra button (gamepads::Extra), by the pad's own brand.
std::string ExtraButtonName(const gamepads::PadView& pad, unsigned extra);
// The row id of an extra button's copy choice, and the extra button a row id names.
inline std::string ExtraRowId(unsigned extra) { return std::string("extra-") + gamepads::ExtraIds[extra]; }
inline std::optional<unsigned> ExtraOfRow(const std::string& id) {
    for (unsigned i = 0; i < gamepads::ExtraCount; ++i) if (id == ExtraRowId(i)) return i;
    return {};
}
// How a capture shows on the rows: the row waiting for a press reads
// button_layout.waiting in Ember and every other row dims; the row that just
// took a press shows its value in Ember for a moment. Empty ids mark nothing.
struct LayoutRowMarks { std::string waiting, flash; };
std::vector<MenuEntry> ButtonLayoutRows(const gamepads::PadView& pad, bool editable, const LayoutRowMarks& marks = {});
std::vector<MenuEntry> ButtonEditorRows(const gamepads::PadView& pad, unsigned button, bool editable);
void DrawButtonLayoutDetail(const gamepads::PadView& pad, gamepads::ButtonPrompts prompts, bool capture);
// The card over the list while a capture waits: what to press (press_for, or
// press_to_change for a pick), the controller's name, a bar that empties over
// the capture's timeout (left, 1 to 0) and how to cancel.
struct CaptureCard { std::string prompt, pad; double left = 1; };
CaptureCard LayoutCaptureCard(const gamepads::PadView& pad, const std::optional<std::uint32_t>& mask, double left);
// The same card for the Menu confirm (back false) or Menu back row.
CaptureCard MenuCaptureCard(const gamepads::PadView& pad, bool back, double left);
// The same card for the Training record (play false) or Training play row.
CaptureCard TrainingCaptureCard(const gamepads::PadView& pad, bool play, double left);
// The pad's name for a Training button, Off for none, and a note when the
// layout gives that button an attack, which keeps it from working.
std::string TrainingButtonName(const gamepads::PadView& pad, int button);
float CaptureCardHeight(const CaptureCard& card, float width);
void DrawCaptureCard(const CaptureCard& card);
void DrawButtonEditorDetail(const gamepads::PadView& pad, gamepads::ButtonPrompts prompts, unsigned button);
} }
