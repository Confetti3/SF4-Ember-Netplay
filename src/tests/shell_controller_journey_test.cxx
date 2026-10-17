#include "shell_journey_support.hxx"
#include "../ui/ControllersScreen.hxx"
namespace {
// The summary's Select and Back are the newly recorded LK and MK, and its
// legend names them, while the stick's installed menu buttons (buttons 3 and
// 2: its record is unknown, so the game's generic default) do nothing until
// Save; after it, ordinary menus take the installed buttons again.
void GameplayTargetSummary() {
    using namespace sf4e; using namespace gamepads;
    Harness h;
    PadView pad; pad.id = 11; pad.name = "Unmapped DirectInput stick";
    pad.identity = {"direct-guid", "", "direct-path", pad.name};
    pad.state = RowState::Unmapped; pad.gameplay = true;
    h.view.pads = {pad}; h.view.inputDevice.type = input::PadDirectInput;
    h.view.padSetup.identity = pad.identity;
    auto& setup = h.view.padSetup.state;
    setup.active = true; setup.mode = SetupMode::Summary;
    for (int i = 0; i < SetupStepCount; ++i) setup.recorded[i] = RawInput{RawKind::Button, i + 10};
    setup.menuDraft = DraftMenu(setup.recorded);
    h.Screen("pad-setup"); h.FocusOn("save");
    ControllerNavigation navigation;
    SetupSummaryControls summary;
    ControllerSample sample; sample.deviceType = input::PadDirectInput; sample.deviceIndex = 5; sample.connected = true;
    sample.menu = input::ResolveMenuButtons(input::PadDirectInput, nullptr, nullptr);
    Check(sample.menu.confirm == 1u << 18 && sample.menu.back == 1u << 17 && !std::strcmp(sample.menu.confirmGlyph, "LK"),
        "A stick with no verified record lost the game's default");
    std::uint64_t now = 1;
    // Exercise the overlay's two input paths: the installed menu buttons and the wizard's new raw bindings.
    const auto tick = [&](unsigned installed, std::vector<RawInput> raw = {}) {
        setup.summaryInput = summary.Read(now += 16, setup.menuDraft, raw);
        sample.buttons = ControllerButtons(0, sample.deviceType, installed, sample.menu);
        const bool targetIsGameplay = std::any_of(h.view.pads.begin(), h.view.pads.end(), [&](const PadView& row) {
            return row.gameplay && Same(row.identity, h.view.padSetup.identity);
        });
        const bool assigning = h.view.inputCapture != input::Capture::Idle;
        navigation.Update(sample, true, true, !assigning && !(setup.active && (setup.mode == SetupMode::Capture || targetIsGameplay)));
        h.Frame(navigation.Buttons());
    };
    std::set<std::string> legend;
    SetMenuTextProbe([&](const char* id, float, float, float, float) { if (!std::strncmp(id, "legend/", 7)) legend.insert(id + 7); });
    tick(0); tick(0);
    const auto advertised = RawInputName(*setup.recorded[LkStep]), advertisedBack = RawInputName(*setup.recorded[MkStep]);
    const auto names = [&](const std::string& text) { return std::any_of(legend.begin(), legend.end(), [&](const std::string& hint) { return hint.find(text) != std::string::npos; }); };
    Check(names(advertised) && names(advertisedBack), "The summary did not advertise the recorded LK and MK");
    SetMenuTextProbe({});
    const auto before = h.actions.size();
    tick(sample.menu.confirm, {{RawKind::Button, 2}}); tick(0);
    Check(h.actions.size() == before && h.shell.Navigation().Focus() == "save", "The installed confirm selected through the gameplay target during summary");
    tick(sample.menu.back, {{RawKind::Button, 1}}); tick(0);
    Check(!h.shell.Navigation().Confirming() && h.shell.Navigation().Focus() == "save", "The installed back opened cancel confirmation during summary");
    tick(0, {*setup.recorded[LkStep]});
    Check(h.actions.size() == before, "New LK selected before release");
    tick(0);
    Check(h.actions.size() == before + 1 && h.actions.back().padAction.command == SetupCommand::Save, "New LK did not select Save on release");
    // Keyboard input has its own path while the target owns controller navigation.
    ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow, true); tick(0);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow, false); tick(0);
    Check(h.shell.Navigation().Focus() == "restart", "Summary target suppressed keyboard navigation");
    // After Save, ordinary menus take the installed buttons and their glyphs.
    setup.active = false; h.view.padSetup.outcome = SetupEnd::Saved; ++h.view.padSetup.revision;
    h.view.menuButtons = sample.menu; h.Frame();
    Check(h.shell.Navigation().Screen() != "pad-setup", "Save did not close the summary");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow, false); NoteMenuDevice(0); NoteMenuDevice(MenuInput::Down); NoteMenuDevice(0);
    legend.clear();
    SetMenuTextProbe([&](const char* id, float, float, float, float) { if (!std::strncmp(id, "legend/", 7)) legend.insert(id + 7); });
    SetMenuGlyphs(input::PadDirectInput, sample.menu); h.Frame();
    SetMenuTextProbe({});
    Check(names("LK") && names("MK") && !names(advertised), "After Save the legend did not name the installed buttons");
    navigation.Reset();
    sample.buttons = 0; navigation.Update(sample, true, true, true); h.Frame(navigation.Buttons());
    sample.buttons = ControllerButtons(0, sample.deviceType, sample.menu.back, sample.menu);
    Check(sample.buttons == ControllerSample::Back, "The installed back is not Back after Save");
}
// Another gameplay pad keeps its own navigation while a summary runs.
void OtherPadDuringSummary() {
    using namespace sf4e; using namespace gamepads;
    Harness h;
    PadView pad; pad.id = 11; pad.name = "Unmapped DirectInput stick";
    pad.identity = {"direct-guid", "", "direct-path", pad.name}; pad.state = RowState::Unmapped;
    auto other = pad; other.id = 12; other.identity.path = "other-path"; other.gameplay = true;
    h.view.pads = {pad, other}; h.view.inputDevice.type = input::PadXInput; h.view.padSetup.identity = pad.identity;
    auto& setup = h.view.padSetup.state; setup.active = true; setup.mode = SetupMode::Summary;
    for (int i = 0; i < SetupStepCount; ++i) setup.recorded[i] = RawInput{RawKind::Button, i + 10};
    setup.menuDraft = DraftMenu(setup.recorded);
    h.Screen("pad-setup"); h.FocusOn("restart");
    ControllerNavigation navigation;
    ControllerSample sample; sample.deviceType = input::PadXInput; sample.deviceIndex = 0; sample.connected = true;
    const auto tick = [&](unsigned held) { sample.buttons = ControllerButtons(held, sample.deviceType, 0, sample.menu); navigation.Update(sample, true, true, true); h.Frame(navigation.Buttons()); };
    tick(0); tick(0); tick(0x2); tick(0);
    Check(h.shell.Navigation().Focus() == "cancel", "A different gameplay pad lost summary navigation");
}
void SetupJourneys() {
    using namespace sf4e; using namespace gamepads;
    Harness h; h.view.inputDevice.type = input::PadXInput;
    h.view.gameplayBrand = h.view.promptBrand = {Brand::PlayStation, PlayStationArt::PS5};
    PadView pad; pad.id = 9; pad.name = "Unknown fight stick"; pad.picture = "controller_generic";
    pad.identity = {"prompt-guid", "", "prompt-path", pad.name}; pad.state = RowState::Unmapped;
    h.view.pads = {pad}; h.Frame();
    h.view.pads[0].raw = {{RawKind::Button, 0}}; h.view.pads[0].pressed = true; h.Frame();
    Check(h.actions.back().padAction.kind == ControllerRequest::Kind::Prompt, "First press did not show automatic setup prompt");
    Check(ImGui::GetTopMostPopupModal(), "Automatic setup prompt did not own the menu");
    h.Frame(0, 3); Check(h.shell.Navigation().Screen() == "home", "Triggering press opened setup without a release");
    h.Press(MenuInput::Back);
    Check(h.actions.back().padAction.kind == ControllerRequest::Kind::DeclinePrompt, "Prompt Back did not decline");
    const auto declined = h.actions.size();
    h.view.pads[0].pressed = false; h.view.pads[0].raw.clear(); h.Frame();
    h.view.pads[0].pressed = true; h.view.pads[0].raw = {{RawKind::Button, 0}}; h.Frame();
    Check(h.actions.size() == declined && !ImGui::GetTopMostPopupModal(), "Declined pad was prompted twice");
    pad.id = 10; pad.identity.guid = "second-guid"; pad.identity.path = "second-path";
    h.view.pads = {pad}; h.Frame();
    h.view.pads[0].pressed = true; h.view.pads[0].raw = {{RawKind::Button, 0}}; h.Frame();
    h.view.pads[0].pressed = false; h.view.pads[0].raw.clear(); h.Frame();
    h.view.pads[0].pressed = true; h.view.pads[0].raw = {{RawKind::Button, 0}}; h.Frame();
    Check(h.shell.Navigation().Screen() == "pad-setup" && h.actions.back().padAction.kind == ControllerRequest::Kind::Setup,
        "Prompt did not open setup after release and the next press");
    h.view.padSetup.padId = 10; h.view.padSetup.identity = pad.identity;
    auto& state = h.view.padSetup.state; state.active = true; h.Frame();
    h.shell.ShowPlay(); h.Frame(); Check(h.shell.Navigation().Screen() == "pad-setup", "Play request displaced active setup");
    h.Press(MenuInput::Down); Check(h.shell.Navigation().Focus() == "step-0", "Capture allowed list navigation");
    state.mode = SetupMode::Summary;
    for (int i = 0; i < SetupStepCount; ++i) state.recorded[i] = RawInput{RawKind::Button, i};
    h.Frame(); state.summaryInput.action = SetupMenuAction::Back; ++state.summaryInput.sequence; h.Frame();
    Check(h.shell.Navigation().Focus() == "cancel" && h.shell.Navigation().Confirming(), "Summary MK did not open cancel confirmation");
    state.summaryInput.up = state.summaryInput.down = false; h.Frame(); h.Press(MenuInput::Back);
    Check(!h.shell.Navigation().Confirming() && h.shell.Navigation().Screen() == "pad-setup", "Summary confirmation Back left setup");
    h.Choose("save"); Check(h.actions.back().padAction.command == SetupCommand::Save, "Summary Save did not submit save");
    state.active = false; h.view.padSetup.outcome = SetupEnd::Saved; ++h.view.padSetup.revision;
    h.view.pads[0].state = RowState::Recognised; h.view.pads[0].slot = 0; h.view.pads[0].customMapping = true; h.Frame();
    Check(h.shell.Navigation().Screen() == "controllers" && h.shell.Navigation().Focus() == "pad-10", "Save did not focus the controller on Controllers");
    Check(ImGui::GetTopMostPopupModal(), "Save did not show its result notice");
    Check(MenuPromptBrand() == h.view.promptBrand, "Closing setup did not restore gameplay glyphs");
    h.Press(MenuInput::Back); h.Choose("pad-10"); h.Press(MenuInput::Right); h.Press(MenuInput::Right); h.Press(MenuInput::Select);
    state = {}; state.active = true; h.Frame();
    state.mode = SetupMode::Summary; for (int i = 0; i < SetupStepCount; ++i) state.recorded[i] = RawInput{RawKind::Button, i}; h.Frame();
    state.summaryInput.action = SetupMenuAction::Back; ++state.summaryInput.sequence; h.Frame();
    Check(h.shell.Navigation().Confirming(), "Cancel confirmation did not open before hiding");
    h.shell.Background(h.view, h.view.room, [&](ShellAction action) { h.actions.push_back(std::move(action)); return true; });
    Check(h.actions.back().padAction.command == SetupCommand::LeftMenu && !h.shell.NoticeOpen(), "Hidden wizard did not cancel silently");
    h.shell.ShowPlay(); h.shell.ShowPlay(); h.Frame();
    Check(h.shell.Navigation().Screen() == "controllers", "Reopening lost the silent cancel destination");
}
}
// Outside the wizard the game holds an unmapped pad, so its press arrives as
// the game's own DirectInput word: pressed with no raw inputs.
static void GamePressPrompt() {
    using namespace sf4e; using namespace gamepads;
    Harness h; h.view.inputDevice.type = input::PadXInput;
    PadView pad; pad.id = 12; pad.name = "Game-held stick"; pad.picture = "controller_generic";
    pad.identity = {"held-guid", "", "held-path", pad.name}; pad.state = RowState::Unmapped; pad.directInputIndex = 4;
    h.view.pads = {pad}; h.Frame();
    h.view.pads[0].pressed = true; h.Frame();
    Check(h.actions.back().padAction.kind == ControllerRequest::Kind::Prompt, "A press read by the game did not prompt setup");
    h.Frame(0, 3); Check(h.shell.Navigation().Screen() == "home", "The prompting press opened setup without a release");
    h.view.pads[0].pressed = false; h.Frame();
    h.view.pads[0].pressed = true; h.Frame();
    Check(h.shell.Navigation().Screen() == "pad-setup" && h.actions.back().padAction.kind == ControllerRequest::Kind::Setup,
        "A release and press read by the game did not open setup");
}
static void ControllerSettingsJourney() {
    using namespace sf4e;
    using namespace gamepads;
    Harness h;
    PadView pad; pad.id = 7; pad.name = "DualSense Wireless Controller"; pad.picture = "controller_playstation5";
    pad.slot = 0; pad.brand = {Brand::PlayStation, PlayStationArt::PS5};
    h.view.pads = {pad}; h.view.controller = "Keyboard";
    std::vector<MenuEntry> rows;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries) { rows = entries; });
    const auto cancelSetup = [&] {
        Check(h.shell.Navigation().Screen() == "pad-setup", "Setup did not open wizard");
        h.view.padSetup.state.active = true; h.Frame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); h.Frame();
        Check(h.actions.back().padAction.command == SetupCommand::Escape, "Capture Esc did not cancel");
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        h.view.padSetup.state.active = false; h.view.padSetup.outcome = SetupEnd::Escape; ++h.view.padSetup.revision; h.Frame();
        Check(h.shell.Navigation().Screen() == "controllers", "Wizard cancel did not return to Controllers");
    };
    const auto row = [&](const char* id) -> const MenuEntry& {
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == id; });
        Check(found != rows.end(), "Controller journey row missing"); return *found;
    };
    h.Screen("settings"); h.Choose("player");
    Check(row("controllers").value == "1 connected", "Player controller count is wrong");
    h.Choose("controllers");
    Check(h.shell.Navigation().Screen() == "controllers", "Controllers did not open from Player");
    Check(row("pad-7").choices.size() == 3, "Recognised controller lacks play and setup choices");
    h.Choose("pad-7");
    Check(h.shell.Navigation().Choosing(), "Controller choice did not open");
    const auto count = h.actions.size(); h.Press(MenuInput::Select);
    Check(!h.shell.Navigation().Choosing() && h.actions.size() == count + 1, "Play did not submit once");
    Check(h.actions.back().padAction.kind == ControllerRequest::Kind::Play && h.actions.back().padAction.id == 7, "Play did not name the selected pad");
    h.view.pads[0].gameplay = true; h.Frame();
    Check(!row("pad-7").choices[0].enabled && row("pad-7").choices[0].detail == loc::T("controllers.choice_play_current"), "Current pad play rule is wrong");
    h.Choose("pad-7"); h.Press(MenuInput::Right); h.Press(MenuInput::Right); h.Press(MenuInput::Select);
    Check(h.actions.back().padAction.kind == ControllerRequest::Kind::Setup, "Setup choice did not use the shared entry point");
    cancelSetup();
    h.view.pads[0].gameplay = false; h.view.canChangeController = h.view.canEditPreferences = false; h.Frame();
    Check(!row("pad-7").choices[0].enabled && row("pad-7").choices[0].detail == loc::T("settings.leave_room_to_edit"), "Room did not lock Play");
    Check(!row("pad-7").choices[2].enabled, "Room did not lock Setup");
    h.view.canChangeController = h.view.canEditPreferences = true;
    h.view.pads[0].state = RowState::Unmapped; h.Frame(); h.Choose("pad-7");
    Check(h.actions.back().padAction.kind == ControllerRequest::Kind::Setup, "Unmapped row did not use the shared setup entry point");
    cancelSetup();
    h.view.pads[0].state = RowState::Recognised; h.view.pads[0].customMapping = true; h.Frame();
    Check(row("pad-7").choices.size() == 4 && row("pad-7").choices[3].id == "forget", "Saved mapping did not offer Forget");
    h.Choose("pad-7"); h.Press(MenuInput::Back);
    Check(h.shell.Navigation().Screen() == "controllers" && !h.shell.Navigation().Choosing(), "Choice Back did not cancel");
    h.Press(MenuInput::Back);
    Check(h.shell.Navigation().Screen() == "player", "Controllers Back did not return to Player");
    h.FocusOn("game-reads-pads");
    const auto before = h.actions.size(); h.Press(MenuInput::Select);
    Check(h.actions.size() == before, "Select activated the game reads setting");
    h.Press(MenuInput::Right);
    Check(row("game-reads-pads").value == "On" && row("game-reads-pads").detail.find(loc::T("player.game_reads_pending")) != std::string::npos, "Opt-out did not show On and pending");
    Check(h.actions.size() == before, "Opt-out bypassed normal autosave");
    h.Frame(0, 45);
    Check(h.actions.back().command.kind == Kind::SavePreferences && !h.actions.back().preferences.controllerLayer, "Opt-out did not autosave the inverse layer setting");
    h.view.preferences = h.actions.back().preferences; h.Frame();
    Check(row("game-reads-pads").detail.find(loc::T("player.game_reads_pending")) != std::string::npos, "Saved opt-out lost pending before restart");
    h.Press(MenuInput::Left); h.Frame(0, 45);
    Check(h.actions.back().preferences.controllerLayer && row("game-reads-pads").value == "Off", "Left did not restore the layer setting");
    Check(row("game-reads-pads").detail.find(loc::T("player.game_reads_pending")) == std::string::npos, "Returning to running mode left pending text");
    h.view.preferences = h.actions.back().preferences; h.Frame();
    h.view.canEditPreferences = false; h.Frame();
    Check(!row("game-reads-pads").enabled && row("game-reads-pads").detail == loc::T("settings.edit_at_main_menu"), "Locked opt-out has the wrong detail");
    const auto locked = h.actions.size(); h.Press(MenuInput::Right); h.Frame(0, 45);
    Check(h.actions.size() == locked, "Locked opt-out changed preferences");
    SetMenuEntriesProbe({});
}
void ControllerJourneys() { ControllerSettingsJourney(); SetupJourneys(); GamePressPrompt(); GameplayTargetSummary(); OtherPadDuringSummary(); }
