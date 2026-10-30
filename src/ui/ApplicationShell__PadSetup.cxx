#include "ApplicationShell.hxx"
#include "PadSetupScreen.hxx"

namespace sf4e { namespace ui {
using namespace gamepads;
void ApplicationShell::SetupControl(SetupCommand command, const ShellView& view, const Submit& submit, int step) {
    ShellAction action; action.command.generation = view.session.generation;
    action.padAction = {ControllerRequest::Kind::SetupControl, setupPad_ ? setupPad_->id : 0, command, step};
    if (!submit(std::move(action))) error_ = loc::T("error.queue_failed");
}
void ApplicationShell::BeginPadSetup(const PadView& pad, const ShellView& view, const Submit& submit) {
    if (!view.atMainMenu || !view.canEditPreferences || view.session.room != netplay::RoomState::Idle ||
        view.padLayer != Layer::Active || setupPad_) return;
    ShellAction action; action.command.generation = view.session.generation;
    action.padAction = {ControllerRequest::Kind::Setup, pad.id};
    if (!submit(std::move(action))) { error_ = loc::T("error.queue_failed"); return; }
    setupFromLayout_ = menu_.navigation.Screen() == "button-layout";
    reopenScreen_ = "pad-setup";
    setupPad_ = pad; setupStarting_ = true; setupRevision_ = view.padSetup.revision;
    setupMenuSequence_ = 0; setupPreviousMode_ = SetupMode::Capture; menu_.navigation.Push("pad-setup");
}
void ApplicationShell::ClosePadSetup(const ShellView& view, bool controllers) {
    auto& nav = menu_.navigation;
    controllers = controllers && !setupFromLayout_;
    if (nav.Screen() == "pad-setup") { nav.Cancel(); nav.Return(); }
    if (setupFromLayout_ && view.padSetup.outcome == SetupEnd::Saved) nav.Prefer("test");
    if (controllers && nav.Screen() != "controllers") nav.Push("controllers");
    if (controllers && setupPad_) nav.Focus("pad-" + std::to_string(setupPad_->id), ControllerRows(view.padLayer, view.pads, view.canChangeController, view.canEditPreferences));
    if (reopenScreen_ && *reopenScreen_ == "pad-setup") reopenScreen_.reset();
    setupPad_.reset(); setupStarting_ = false;
    menu_.listEnabled = true; menu_.ownsBack = false; menu_.customDetail = false; menu_.additionalInput = 0; menu_.shortcutHints.clear();
    SetMenuGlyphs(view.inputDevice.type, view.menuButtons, view.promptBrand);
    SetGameplayLayoutPrompts(view);
}
void ApplicationShell::UpdatePadSetup(const ShellView& view, const Submit& submit, bool visible) {
    auto& nav = menu_.navigation;
    if (visible && view.atMainMenu && !setupPad_) reopenScreen_.reset();
    std::set<unsigned> pressed, rising;
    // Outside the wizard SDL leaves an unmapped pad to the game: it has no raw
    // inputs, and pressed is the game's own DirectInput word (ControllerInventory).
    for (const auto& pad : view.pads) if (pad.pressed && (pad.raw.empty() || std::any_of(pad.raw.begin(), pad.raw.end(), [](const RawInput& input) { return input.kind <= RawKind::HatLeft; }))) {
        pressed.insert(pad.id); if (!setupPromptHeld_.count(pad.id)) rising.insert(pad.id);
    }
    setupPromptHeld_ = std::move(pressed);
    if (setupPad_) {
        if (!visible || !view.atMainMenu || view.session.room != netplay::RoomState::Idle) {
            reopenScreen_ = "controllers";
            SetupControl(SetupCommand::LeftMenu, view, submit); ClosePadSetup(view, true); return;
        }
        if (view.padSetup.state.active) setupStarting_ = false;
        if (view.padSetup.revision != setupRevision_) {
            setupRevision_ = view.padSetup.revision;
            const auto outcome = view.padSetup.outcome;
            if (outcome == SetupEnd::SaveFailed) menu_.ShowError(loc::T("pad_setup.save_failed"));
            else if (outcome != SetupEnd::None) {
                const auto name = setupPad_->name;
                const auto savedPad = *setupPad_;
                const bool fromLayout = setupFromLayout_;
                ClosePadSetup(view, outcome == SetupEnd::Saved);
                if (outcome == SetupEnd::Saved) {
                    if (fromLayout) menu_.ShowNotice(loc::Tf("pad_setup.saved", name), loc::T("pad_setup.saved_heading"));
                    else {
                        dialogContext_.pad = savedPad;
                        menu_.ShowNotice(NoticeDialog{loc::Tf("pad_setup.saved", name), loc::T("pad_setup.saved_heading"),
                            {{loc::T("common.ok")}, {loc::T("player.button_layout"), NoticeResult::OpenButtonLayout}}});
                    }
                }
                else if (outcome == SetupEnd::Disconnect) menu_.ShowNotice(loc::Tf("pad_setup.stopped_disconnect", name));
                else if (outcome == SetupEnd::Idle) menu_.ShowNotice(loc::T("pad_setup.stopped_idle"));
                else if (outcome != SetupEnd::LeftMenu) { notice_ = loc::T("pad_setup.cancelled"); noticeTone_ = Tone::Neutral; noticeUntil_ = ImGui::GetTime() + 4; }
            }
        }
        return;
    }
    if (setupPrompt_) {
        if (!visible || !view.atMainMenu || !menu_.NoticeOpen()) {
            ShellAction action; action.command.generation = view.session.generation; action.padAction.kind = ControllerRequest::Kind::DeclinePrompt;
            submit(std::move(action)); menu_.CloseNotice(); setupPrompt_.reset(); return;
        }
        const auto pad = std::find_if(view.pads.begin(), view.pads.end(), [&](const PadView& p) { return Same(p.identity, setupPrompt_->identity); });
        if (pad != view.pads.end()) {
            const bool rest = pad->rawAtRest && !pad->pressed;
            if (rest) setupPromptReleased_ = true;
            else if (setupPromptReleased_ && rising.count(pad->id)) { const auto selected = *pad; menu_.CloseNotice(); setupPrompt_.reset(); BeginPadSetup(selected, view, submit); }
        }
        return;
    }
    if (!visible || !view.atMainMenu || !view.canEditPreferences || view.session.room != netplay::RoomState::Idle ||
        nav.Screen().compare(0, 4, "room") == 0 || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
        view.inputCapture != input::Capture::Idle || nav.Mode() != MenuNavigation::Modal::None || menu_.NoticeOpen() || nav.Screen() == "pad-setup") return;
    for (const auto& pad : view.pads) {
        const bool press = rising.count(pad.id) != 0;
        if (pad.state != RowState::Unmapped || !press || std::any_of(setupPrompted_.begin(), setupPrompted_.end(), [&](const Identity& identity) { return Same(identity, pad.identity); })) continue;
        setupPrompted_.push_back(pad.identity); setupPrompt_ = pad; setupPromptReleased_ = false;
        NoteUserText(pad.name.c_str());
        menu_.ShowNotice(loc::Tf("pad_setup.prompt_text", pad.name), loc::T("pad_setup.prompt_heading"), loc::T("pad_setup.not_now"));
        ShellAction action; action.command.generation = view.session.generation; action.padAction = {ControllerRequest::Kind::Prompt, pad.id}; submit(std::move(action));
        break;
    }
}
bool ApplicationShell::DrawPadSetup(const ShellView& view, const Submit& submit) {
    auto& nav = menu_.navigation;
    if (nav.Screen() != "pad-setup") return false;
    // UiRender uses the same copied snapshot as the overlay.
    if (!setupPad_) {
        const auto pad = std::find_if(view.pads.begin(), view.pads.end(), [&](const PadView& p) { return Same(p.identity, view.padSetup.identity); });
        if (pad == view.pads.end()) return false;
        setupPad_ = *pad; setupRevision_ = view.padSetup.revision; setupMenuSequence_ = view.padSetup.state.summaryInput.sequence;
    }
    auto state = view.padSetup.state;
    if (setupStarting_) { state = {}; state.active = true; }
    const bool capture = state.mode == SetupMode::Capture;
    const auto rows = PadSetupRows(state);
    if (capture || setupPreviousMode_ == SetupMode::Capture) nav.Focus("step-" + std::to_string(state.step), rows);
    setupPreviousMode_ = state.mode;
    menu_.listEnabled = !capture; menu_.ownsBack = true; menu_.customDetail = true; menu_.additionalInput = (state.summaryInput.up ? MenuInput::Up : 0) | (state.summaryInput.down ? MenuInput::Down : 0);
    if (!capture && state.summaryInput.sequence != setupMenuSequence_) {
        menu_.additionalInput |= state.summaryInput.action == SetupMenuAction::Select ? MenuInput::Select :
            state.summaryInput.action == SetupMenuAction::Back ? MenuInput::Back : 0;
        setupMenuSequence_ = state.summaryInput.sequence;
    }
    menu_.shortcutHints = capture ? PadSetupLegend(state) : std::vector<LegendHint>{};
    menu_.backHint = capture ? loc::T("common.cancel") : loc::T("pad_setup.cancel");
    menu_.footerNote.clear(); menu_.typesText = false;
    if (capture) SetMenuGlyphs(input::PadKeyboard, 0, 0);
    else {
        // The draft's own inputs select and go back here, whatever the
        // installed buttons are, so the prompts name them (SetupMenuDraft).
        const auto& draft = state.menuDraft;
        const auto select = draft.select ? RawInputName(*draft.select) : std::string("LK");
        const auto back = draft.back ? RawInputName(*draft.back) : std::string("MK");
        SetMenuGlyphs(input::PadDirectInput, 0, 0, select.c_str(), back.c_str(), {Brand::Stick});
        SetMenuPromptBrand({Brand::Stick}, NamesOf(setupPad_->table));
    }
    SetupCommand command = SetupCommand::None; int step = 0;
    if (capture && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) command = SetupCommand::Escape;
    else if (capture && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) command = SetupCommand::Backspace;
    else if (capture && ImGui::IsKeyPressed(ImGuiKey_Space, false)) command = SetupCommand::Space;
    else if (KeyboardMenuBits()) command = SetupCommand::Activity;
    const auto action = menu_.Draw(loc::T("pad_setup.title"), rows, "", [&](const std::string& id) {
        DrawPadSetupDetail(*setupPad_, state);
        if (!capture) {
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& entry) { return entry.id == id; });
            if (row != rows.end()) ImGui::TextWrapped("%s", row->detail.c_str());
        }
    });
    AnswerDialog(submit);
    if (action.kind == MenuAction::Back) {
        if (capture) command = SetupCommand::Cancel;
        else { nav.Focus("cancel", rows); nav.Choose(rows); }
    } else if (action.kind == MenuAction::Activate) {
        if (action.id == "save") command = SetupCommand::Save;
        else if (action.id == "restart") command = SetupCommand::Restart;
        else if (action.id == "cancel") command = SetupCommand::Cancel;
        else if (action.id.compare(0, 5, "step-") == 0) { command = SetupCommand::Redo; step = std::stoi(action.id.substr(5)); }
    }
    if (command == SetupCommand::None && menu_.Held()) command = SetupCommand::Activity;
    if (command != SetupCommand::None) SetupControl(command, view, submit, step);
    return true;
}
} }
