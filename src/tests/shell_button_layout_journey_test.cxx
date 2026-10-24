#include "shell_journey_support.hxx"
#include "../ui/ButtonLayoutScreen.hxx"

namespace {
using namespace sf4e;
using namespace gamepads;
PadView LayoutPad() {
    PadView pad; pad.id = 27; pad.slot = 0; pad.gameplay = true;
    pad.identity = {"layout-guid", "layout-serial", "layout-path", "DualSense Wireless Controller"};
    pad.name = pad.identity.name; pad.picture = "controller_playstation5"; pad.brand = {Brand::PlayStation, PlayStationArt::PS5};
    return pad;
}
void Prepare(Harness& h) {
    h.view.padLayer = Layer::Active; h.view.inputDevice.type = input::PadXInput;
    h.view.pads = {LayoutPad()}; h.view.controller = h.view.pads[0].name;
    h.view.gameplayBrand = h.view.promptBrand = h.view.pads[0].brand;
    h.Screen("player"); h.Choose("controls");
    Check(h.shell.Navigation().Screen() == "button-layout" && h.shell.Navigation().Focus() == "test", "Player did not open Button layout on test");
}
const ControllerRequest& LastLayout(const Harness& h) {
    const auto at = std::find_if(h.actions.rbegin(), h.actions.rend(), [](const ShellAction& action) { return action.padAction.kind == ControllerRequest::Kind::Layout; });
    Check(at != h.actions.rend(), "No layout submitted"); return at->padAction;
}
void LayoutAssignPresetReset() {
    Harness h; Prepare(h);
    h.view.pads[0].held = {"A"}; h.Choose("fn-lp");
    h.Frame(0, 3); Check(std::none_of(h.actions.begin(), h.actions.end(), [](const ShellAction& a) { return a.padAction.kind == ControllerRequest::Kind::Layout; }), "Opening A assigned itself");
    h.view.pads[0].held.clear(); h.Frame(); h.view.pads[0].held = {"Y"}; h.Frame();
    const auto assigned = LastLayout(h);
    Check(assigned.id == 27 && assigned.buttons[2] == 0x20 && assigned.buttons[3] == 0x10, "Capture did not swap LP onto Triangle");
    Check(assigned.log.find("fn-lp") != std::string::npos, "Assign log omitted its row");
    h.view.pads[0].held.clear(); h.Frame(); h.Wait(.5f);
    Check(h.actions.back().command.kind == Kind::SavePreferences && h.actions.back().preferences.buttonLayouts[0].buttons == assigned.buttons,
        "Layout did not reach autosave after applying");
    h.view.preferences = h.actions.back().preferences; h.view.settingsPending = true; h.Frame(); h.view.settingsPending = false; h.Frame();
    h.Choose("preset"); h.Press(MenuInput::Down); h.Press(MenuInput::Select);
    Check(LastLayout(h).buttons == LayoutPresets[1].buttons, "Triggers preset did not apply");
    h.Choose("preset"); h.Press(MenuInput::Up); h.Press(MenuInput::Select);
    Check(LastLayout(h).buttons == DefaultLayout, "Default preset did not reset");
    h.Choose("remove"); h.Press(MenuInput::Right); h.Press(MenuInput::Select);
    Check(h.actions.back().padAction.kind == ControllerRequest::Kind::RemoveLayout && h.shell.Navigation().Focus() == "preset", "Use game's layout did not remove and focus preset");
    h.Press(MenuInput::Back); Check(h.shell.Navigation().Screen() == "player", "Remove left an unwanted missing-attack warning");
    h.Choose("game-buttons"); Check(h.actions.back().command.kind == Kind::StartOffline, "Game's button settings did not send StartOffline");
}
void LayoutEditorWarningRoom() {
    Harness h; Prepare(h);
    h.Choose("edit-button"); h.view.pads[0].held = {"X"}; h.Frame();
    Check(h.shell.Navigation().Screen() == "button-layout-button" && h.shell.Navigation().Focus() == "bit-lp", "Pick capture did not open the button editor");
    h.view.pads[0].held.clear(); h.Frame(); h.Choose("clear");
    Check(LastLayout(h).buttons[2] == 0, "Clear did not empty X");
    h.Press(MenuInput::Back); Check(h.shell.Navigation().Screen() == "button-layout" && !ImGui::GetTopMostPopupModal(), "Returning from editor opened the warning");
    h.Press(MenuInput::Back); Check(ImGui::GetTopMostPopupModal(), "Missing LP did not open leaving warning");
    h.Press(MenuInput::Select); Check(h.shell.Navigation().Screen() == "button-layout", "Keep editing was not the default");
    h.Press(MenuInput::Back); h.Press(MenuInput::Right); h.Press(MenuInput::Select);
    Check(h.shell.Navigation().Screen() == "player", "Leave did not return to Player");
    Check(h.actions.back().padAction.log.find("left with no button for LP") != std::string::npos, "Leave warning log missing");
    h.Choose("controls"); h.Press(MenuInput::Back);
    Check(h.shell.Navigation().Screen() == "player", "Unchanged visit warned again");
}
void LayoutRoom() {
    Harness room;
    Prepare(room); room.Press(MenuInput::Back);
    room.view.canEditPreferences = false; JoinedRoom(room, 42); room.Frame(); room.Screen("player"); room.Choose("controls");
    room.Choose("fn-lp"); room.view.pads[0].held = {"RB"}; room.Frame(); room.view.pads[0].held.clear(); room.Frame();
    Check(LastLayout(room).buttons[5] == 0x10, "Room layout edit was locked");
    room.view.canEditMusic = true;
    room.Screen("music"); room.Press(MenuInput::Right);
    const auto saves = [&] { return std::count_if(room.actions.begin(), room.actions.end(),
        [](const ShellAction& action) { return action.saveLocalPresentation; }); };
    const auto queued = saves();
    room.Wait(.5f);
    Check(saves() == queued + 1 && room.actions.back().saveLocalPresentation &&
        !room.actions.back().preferences.buttonLayouts.empty() && room.actions.back().preferences.lobbyMusic == sf4e::music::Random,
        "Music and layout edits did not autosave together in one local request");
}
void LayoutCancelsAndEdges() {
    for (const auto* cancel : {"Start", "up"}) {
        Harness h; Prepare(h); h.Choose("fn-lp"); h.view.pads[0].held = {cancel}; h.Frame();
        Check(h.actions.back().padAction.log.find("cancelled") != std::string::npos, "Target cancel did not stop capture");
        Check(std::none_of(h.actions.begin(), h.actions.end(), [](const ShellAction& a) { return a.padAction.kind == ControllerRequest::Kind::Layout; }), "Cancel changed layout");
    }
    {
        Harness escape; Prepare(escape); escape.Choose("fn-lp");
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); escape.Frame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); escape.Frame();
        Check(escape.actions.back().padAction.log.find("cancelled (escape)") != std::string::npos, "Escape did not cancel layout capture");
    }
    Harness h; Prepare(h); h.Choose("fn-lp"); h.Wait(10.1f);
    Check(h.actions.back().padAction.log.find("timeout") != std::string::npos, "Capture did not time out");
    h.Choose("fn-lp"); h.view.pads.clear(); h.Frame();
    Check(h.shell.Navigation().Focus() == "layout-absent", "Disconnect did not focus Not connected");
    Check(h.actions.back().padAction.log.find("assign fn-lp cancelled (state)") != std::string::npos, "Disconnect capture lost the original function row");
    h.view.pads = {LayoutPad()}; h.Frame(); Check(h.shell.Navigation().Focus() == "test", "Reconnect did not focus test");
    h.view.canEditPreferences = false; h.Frame();
    const auto readonly = ButtonLayoutRows(h.view.pads[0], false);
    Check(readonly[0].id == "test" && std::all_of(readonly.begin() + 1, readonly.end(), [](const MenuEntry& row) { return !row.enabled; }), "Read-only rows were editable");
    const auto before = h.actions.size(); h.Choose("fn-lp"); h.view.pads[0].held = {"Y"}; h.Frame();
    Check(h.actions.size() == before, "Read-only screen opened capture");
}
void WizardLayoutEntry() {
    Harness h;
    auto pad = LayoutPad(); pad.state = RowState::Unmapped; pad.gameplay = false;
    h.view.pads = {pad}; h.view.padLayer = Layer::Active;
    h.Screen("controllers"); h.Choose("pad-27");
    h.view.padSetup.identity = pad.identity; h.view.padSetup.padId = pad.id;
    auto& state = h.view.padSetup.state; state.active = true; state.mode = SetupMode::Summary;
    for (int i = 0; i < SetupStepCount; ++i) state.recorded[i] = RawInput{RawKind::Button, i};
    h.Frame(); h.Choose("save");
    h.view.preferences.controllerMappings[pad.identity.guid] = SetupMapping(state, SdlSnapshot{}, pad.identity.guid, pad.name, {}).value();
    h.view.pads[0].state = RowState::Recognised; h.view.pads[0].brand = {Brand::Stick}; h.view.pads[0].slot = 0;
    h.view.pads[0].has[8] = h.view.pads[0].has[9] = false;
    state.active = false; h.view.padSetup.outcome = SetupEnd::Saved; ++h.view.padSetup.revision; h.Frame();
    h.Press(MenuInput::Right); h.Press(MenuInput::Select);
    Check(h.shell.Navigation().Screen() == "button-layout" && h.shell.Navigation().Focus() == "test", "Wizard alternative did not open Button layout");
    h.Press(MenuInput::Back); Check(h.shell.Navigation().Screen() == "controllers", "Wizard layout did not return to Controllers");
    h.view.pads[0].state = RowState::GameRead; h.view.pads[0].gameplay = true; h.view.inputDevice.type = input::PadXInput;
    h.Screen("player"); h.Choose("controls"); h.Choose("setup");
    state = {}; state.active = true; state.mode = SetupMode::Summary;
    for (int i = 0; i < SetupStepCount; ++i) state.recorded[i] = RawInput{RawKind::Button, i};
    h.Frame(); h.Choose("save"); h.view.pads[0].state = RowState::Recognised;
    state.active = false; h.view.padSetup.outcome = SetupEnd::Saved; ++h.view.padSetup.revision; h.Frame();
    Check(h.shell.Navigation().Screen() == "button-layout" && h.shell.Navigation().Focus() == "test", "Setup-first save did not return to Button layout on test");
    h.Press(MenuInput::Back); Check(h.shell.Navigation().Screen() == "button-layout", "Setup-first saved notice changed screens");
}
void TargetNavigation() {
    Harness h; Prepare(h); h.view.pads[0].gameplay = false; h.Frame();
    h.view.pads[0].held = {"down"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    Check(h.shell.Navigation().Focus() == "fn-lp", "Target pad did not navigate its own layout");
    h.view.pads[0].held = {"A"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    h.view.pads[0].held = {"B"}; h.Frame();
    Check(LastLayout(h).buttons[1] == 0x10 && h.shell.Navigation().Screen() == "button-layout", "Target B was not bindable or also went back");
    h.view.pads[0].held.clear(); h.Frame();
}

void GuidFallbackEdit() {
    Harness h; auto pad = LayoutPad(); pad.identity.serial.clear(); pad.identity.path = "new-port";
    auto saved = pad.identity; saved.path = "old-port";
    h.view.padLayer = Layer::Active; h.view.inputDevice.type = input::PadXInput; h.view.pads = {pad};
    h.view.preferences.buttonLayouts = {{saved, DefaultLayout, 1}};
    h.Screen("player"); h.Choose("controls"); h.Choose("fn-lp");
    h.view.pads[0].held = {"Y"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame(); h.Wait(.5f);
    Check(h.actions.back().preferences.buttonLayouts.size() == 1 && h.actions.back().preferences.buttonLayouts[0].identity.path == "new-port",
        "Guid fallback edit duplicated the layout or saved the old port");
    h.view.preferences = h.actions.back().preferences; h.Frame(); h.Choose("remove"); h.Press(MenuInput::Right); h.Press(MenuInput::Select); h.Wait(.5f);
    Check(h.actions.back().preferences.buttonLayouts.empty(), "Guid fallback layout could not be removed");
}
// Red Focus (LP+MP+MK) has its own row. While a capture waits, its row reads
// "Press a button..." in Ember and the others dim; the row then flashes and
// the status reads as a success.
void RedFocusAndCaptureRows() {
    Check(AttackLongName(0xB0) == loc::T("button_layout.red_focus"), "0xB0 has no long name");
    Harness h; std::vector<MenuEntry> rows; std::string status; Tone tone = Tone::Neutral;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r) { rows = r; });
    SetMenuStatusProbe([&](const char* text, Tone t) { status = text; tone = t; });
    const auto row = [&](const char* id) {
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
        Check(at != rows.end(), "Button layout row missing"); return *at;
    };
    auto pad = LayoutPad(); pad.table[2] = 0xB0; Prepare(h); h.view.pads = {pad}; h.Frame();
    const auto focus = std::find_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "fn-focus"; });
    Check(focus != rows.end() && focus + 1 != rows.end() && focus[1].id == "fn-red-focus", "Red Focus does not follow Focus");
    Check(row("fn-red-focus").label == loc::T("button_layout.red_focus") && row("fn-red-focus").value == "Square", "Red Focus row does not show its button");
    h.Choose("fn-red-focus");
    Check(row("fn-red-focus").value == loc::T("button_layout.waiting") && row("fn-red-focus").valueHighlighted && row("fn-red-focus").enabled,
        "Capture did not show the waiting value on its row");
    Check(!row("fn-lp").enabled && !row("preset").enabled && !row("test").enabled, "Capture did not dim the other rows");
    h.view.pads[0].held = {"RB"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    Check(LastLayout(h).buttons[5] == 0xB0 && LastLayout(h).buttons[2] == 0x400, "Red Focus did not swap onto R1");
    Check(row("fn-red-focus").value == "R1" && row("fn-red-focus").valueHighlighted && row("fn-lp").enabled, "Assigned row did not flash with its new button");
    Check(status.find(loc::T("button_layout.red_focus")) != std::string::npos && tone == Tone::Success, "Assignment status is not a success naming Red Focus");
    h.Wait(.7f); Check(!row("fn-red-focus").valueHighlighted, "Flash did not end");
    h.Choose("edit-button");
    Check(row("edit-button").value == loc::T("button_layout.waiting") && !row("fn-red-focus").enabled, "Pick capture did not wait on Change one button");
    h.view.pads[0].held = {"Start"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    Check(row("edit-button").value.empty() && row("fn-lp").enabled, "Cancelled capture left the waiting value");
    SetMenuEntriesProbe({}); SetMenuStatusProbe({});
}
// Menu confirm and Menu back: a press binds the button for this controller,
// the other row's button is refused, presets keep the binding, and the legend
// and the target pad follow the answer the runtime resolves from it.
void MenuButtonRows() {
    Harness h; std::vector<MenuEntry> rows; std::string status; Tone tone = Tone::Neutral;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r) { rows = r; });
    SetMenuStatusProbe([&](const char* text, Tone t) { status = text; tone = t; });
    const auto row = [&](const char* id) {
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
        Check(at != rows.end(), "Menu button row missing"); return *at;
    };
    Prepare(h);
    Check(row("menu-confirm").value == "Cross" && row("menu-back").value == "Circle", "Menu rows do not show the game's buttons by default");
    const auto layouts = [&] { return std::count_if(h.actions.begin(), h.actions.end(), [](const ShellAction& a) { return a.padAction.kind == ControllerRequest::Kind::Layout; }); };
    h.Choose("menu-confirm"); h.view.pads[0].held = {"B"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    Check(layouts() == 0 && tone == Tone::Error && status.find(loc::T("button_layout.menu_back")) != std::string::npos,
        "Menu confirm took the Menu back button");
    h.Choose("menu-confirm"); h.view.pads[0].held = {"X"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    Check(LastLayout(h).bound[MenuConfirmButton] == 2 && LastLayout(h).bound[MenuBackButton] == -1 && LastLayout(h).buttons == DefaultLayout,
        "Menu confirm did not bind Square alone");
    Check(tone == Tone::Success && status.find(loc::T("button_layout.menu_confirm")) != std::string::npos, "Binding did not say so");
    h.Wait(.5f);
    Check(h.actions.back().preferences.buttonLayouts.size() == 1 && h.actions.back().preferences.buttonLayouts[0].bound[MenuConfirmButton] == 2, "Binding did not autosave");
    h.view.preferences = h.actions.back().preferences;
    // The runtime resolves the binding on its next frame.
    h.view.pads[0].bound[MenuConfirmButton] = 2; h.view.pads[0].menuButtons = input::ResolveMenuButtons(input::PadXInput, nullptr, nullptr,
        input::MenuConfirmFunction, input::MenuBackFunction, LayoutButtonMask(2), 0);
    h.view.menuButtons = h.view.pads[0].menuButtons; SetMenuGlyphs(input::PadXInput, h.view.menuButtons, h.view.promptBrand); h.Frame();
    Check(row("menu-confirm").value == "Square", "Menu confirm row does not show its binding");
    h.Choose("menu-back"); h.view.pads[0].held = {"X"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    Check(LastLayout(h).bound[MenuBackButton] == -1 && tone == Tone::Error, "Menu back took the bound Menu confirm button");
    h.Choose("preset"); h.Press(MenuInput::Down); h.Press(MenuInput::Select);
    Check(LastLayout(h).buttons == LayoutPresets[1].buttons && LastLayout(h).bound[MenuConfirmButton] == 2, "A preset dropped the menu binding");
    // A target pad that is not the gameplay controller selects with its bound button.
    h.view.pads[0].gameplay = false; h.Frame(); h.FocusOn("test");
    h.view.pads[0].held = {"down"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    Check(h.shell.Navigation().Focus() == "fn-lp", "Target pad did not move");
    const auto before = h.actions.size();
    h.view.pads[0].held = {"A"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame(); h.Frame(0, 2);
    Check(!row("fn-lp").value.empty() && row("fn-lp").value != loc::T("button_layout.waiting"), "A still selected after Menu confirm moved");
    h.view.pads[0].held = {"X"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    Check(row("fn-lp").value == loc::T("button_layout.waiting") && h.actions.size() >= before, "The bound button did not select");
    h.view.pads[0].held = {"Start"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    SetMenuEntriesProbe({}); SetMenuStatusProbe({});
    SetMenuGlyphs(input::PadXInput, input::xinput::A, input::xinput::B);
}
// Extra buttons: only the ones the pad reports get a row, a choice makes one
// copy a layout button, and presets and menu bindings keep the copies.
void ExtraButtonRows() {
    Harness h; std::vector<MenuEntry> rows; std::string status; Tone tone = Tone::Neutral;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r) { rows = r; });
    SetMenuStatusProbe([&](const char* text, Tone t) { status = text; tone = t; });
    const auto has = [&](const std::string& id) { return std::any_of(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; }); };
    const auto row = [&](const char* id) {
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
        Check(at != rows.end(), "Extra button row missing"); return *at;
    };
    Prepare(h);
    Check(!has("extra-touchpad") && !has("extra-home"), "A pad without extra buttons showed their rows");
    h.view.pads[0].hasExtras[static_cast<unsigned>(Extra::Touchpad)] = true; h.view.pads[0].hasExtras[static_cast<unsigned>(Extra::Home)] = true; h.Frame();
    Check(row("extra-touchpad").label == loc::T("button_layout.extra.touchpad") && row("extra-touchpad").value == loc::T("common.off") &&
        row("extra-home").label == "PS" && !has("extra-share"), "Touchpad and PS rows do not show as Off");
    Check(row("extra-touchpad").choices.size() == 13 && row("extra-touchpad").choices[3].label == loc::Tf("button_layout.extra_copy", "Square") &&
        row("extra-touchpad").choices[11].id == "View" && row("extra-touchpad").choices[12].id == "Start",
        "Touchpad choices are not Off, the ten buttons by name, View and Start");
    h.Choose("extra-touchpad"); for (int i = 0; i < 3; ++i) h.Press(MenuInput::Down); h.Press(MenuInput::Select);
    Check(LastLayout(h).extras[static_cast<unsigned>(Extra::Touchpad)] == 2 && LastLayout(h).buttons == DefaultLayout,
        "Touchpad did not copy Square on the game's buttons");
    Check(LastLayout(h).log.find("touchpad copies X") != std::string::npos, "Copy log missing");
    Check(tone == Tone::Success && row("extra-touchpad").value == loc::Tf("button_layout.extra_copy", "Square"), "Touchpad row does not show its copy");
    h.Wait(.5f);
    Check(h.actions.back().preferences.buttonLayouts.size() == 1 && h.actions.back().preferences.buttonLayouts[0].extras[0] == 2, "Copy did not autosave");
    h.view.preferences = h.actions.back().preferences;
    h.view.pads[0].held = {"touchpad"}; h.Frame();
    Check(row("extra-touchpad").valueHighlighted, "Holding the touchpad did not light its row");
    h.view.pads[0].held.clear(); h.Frame();
    h.Choose("preset"); h.Press(MenuInput::Down); h.Press(MenuInput::Select);
    Check(LastLayout(h).buttons == LayoutPresets[1].buttons && LastLayout(h).extras[0] == 2, "A preset dropped the touchpad copy");
    h.Choose("extra-touchpad"); for (int i = 0; i < 3; ++i) h.Press(MenuInput::Up); h.Press(MenuInput::Select);
    Check(LastLayout(h).extras == NoExtraCopies && row("extra-touchpad").value == loc::T("common.off"), "Off did not clear the touchpad copy");
    // View is a copy too: the touchpad can be the pad's Back.
    h.Choose("extra-touchpad"); for (int i = 0; i < 11; ++i) h.Press(MenuInput::Down); h.Press(MenuInput::Select);
    Check(LastLayout(h).extras[static_cast<unsigned>(Extra::Touchpad)] == CopiesView && LastLayout(h).log.find("touchpad copies View") != std::string::npos,
        "Touchpad did not copy View");
    h.Choose("extra-touchpad"); for (int i = 0; i < 11; ++i) h.Press(MenuInput::Up); h.Press(MenuInput::Select);
    Check(LastLayout(h).extras == NoExtraCopies, "Off did not clear the View copy");
    // A capture told the touchpad while it copies nothing says how to give it a press, and takes nothing.
    const auto before = h.actions.size();
    h.Choose("fn-lp"); h.view.pads[0].held = {"touchpad"}; h.Frame(); h.Frame();
    Check(tone == Tone::Error && status.find(loc::Tf("button_layout.extra_uncopied", loc::T("button_layout.extra.touchpad"))) != std::string::npos &&
        row("fn-lp").value == loc::T("button_layout.waiting"), "An uncopied touchpad press did not explain itself while the capture waited");
    h.view.pads[0].held = {"Start"}; h.Frame(); h.view.pads[0].held.clear(); h.Frame();
    Check(std::none_of(h.actions.begin() + static_cast<std::ptrdiff_t>(before), h.actions.end(), [](const ShellAction& a) { return a.padAction.kind == ControllerRequest::Kind::Layout; }),
        "An uncopied touchpad press changed the layout");
    SetMenuEntriesProbe({}); SetMenuStatusProbe({});
}

// Training record and play: a press binds any layout button for this
// controller, even one that attacks, the other row's button is refused,
// presets keep the binding, and Clear turns both off.
void TrainingButtonRows() {
    Harness h; std::vector<MenuEntry> rows; std::string status; Tone tone = Tone::Neutral;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& r) { rows = r; });
    SetMenuStatusProbe([&](const char* text, Tone t) { status = text; tone = t; });
    const auto has = [&](const char* id) { return std::any_of(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; }); };
    const auto row = [&](const char* id) {
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
        Check(at != rows.end(), "Training button row missing"); return *at;
    };
    const auto press = [&](const char* id, const char* glyph) { h.Choose(id); h.view.pads[0].held = {glyph}; h.Frame(); h.view.pads[0].held.clear(); h.Frame(); };
    const auto training = [](const BoundButtons& bound) { return std::make_pair(bound[TrainingRecordButton], bound[TrainingPlayButton]); };
    Prepare(h);
    Check(row("training-record").value == loc::T("common.off") && row("training-play").value == loc::T("common.off") && !has("training-clear"),
        "Training rows do not start Off");
    press("training-record", "X");
    // A button the layout gives an attack binds too: in Training it records instead (TrainingTable).
    Check(LastLayout(h).bound[TrainingRecordButton] == 2 && tone == Tone::Success && row("training-record").value == "Square", "Record did not take a button that attacks");
    press("training-record", "RS");
    Check(LastLayout(h).bound[TrainingRecordButton] == 9 && LastLayout(h).bound[TrainingPlayButton] == -1 && LastLayout(h).buttons == DefaultLayout,
        "Record did not bind the right stick click alone");
    Check(tone == Tone::Success && status.find(loc::T("button_layout.training_record")) != std::string::npos, "Binding did not say so");
    Check(row("training-record").value != loc::T("common.off") && has("training-clear"), "Record row does not show its button");
    h.Wait(.5f);
    Check(h.actions.back().preferences.buttonLayouts.size() == 1 && h.actions.back().preferences.buttonLayouts[0].bound[TrainingRecordButton] == 9, "Binding did not autosave");
    h.view.preferences = h.actions.back().preferences;
    press("training-play", "RS");
    Check(LastLayout(h).bound[TrainingPlayButton] == -1 && tone == Tone::Error, "Play took the Record button");
    press("training-play", "LS");
    Check(training(LastLayout(h).bound) == std::make_pair(9, 8), "Play did not bind the left stick click");
    h.Choose("preset"); h.Press(MenuInput::Down); h.Press(MenuInput::Select);
    Check(LastLayout(h).buttons == LayoutPresets[1].buttons && training(LastLayout(h).bound) == std::make_pair(9, 8), "A preset dropped the Training buttons");
    h.Choose("training-clear"); h.Press(MenuInput::Right); h.Press(MenuInput::Select);
    Check(training(LastLayout(h).bound) == std::make_pair(-1, -1) && row("training-record").value == loc::T("common.off") && !has("training-clear"), "Clear did not turn both Off");
    SetMenuEntriesProbe({}); SetMenuStatusProbe({});
}
}
void ButtonLayoutJourneys() { RedFocusAndCaptureRows(); LayoutAssignPresetReset(); LayoutEditorWarningRoom(); LayoutRoom(); LayoutCancelsAndEdges(); WizardLayoutEntry(); TargetNavigation(); GuidFallbackEdit(); MenuButtonRows(); ExtraButtonRows(); TrainingButtonRows(); }
