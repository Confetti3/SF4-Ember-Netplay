#include "ApplicationShell.hxx"
#include "ButtonLayoutScreen.hxx"
#include <ctime>

namespace sf4e { namespace ui {
using namespace gamepads;
namespace {
bool Served(const PadView& pad) { return pad.state == RowState::Recognised || pad.state == RowState::Steam; }
bool CanEditLayout(const ShellView& view) { return view.atMainMenu && !netplay::Fighting(view.session.match) && view.padLayer == Layer::Active &&
    (view.canEditPreferences || view.session.room != netplay::RoomState::Idle); }
std::vector<Identity> ConnectedLayoutPads(const ShellView& view) {
    std::vector<Identity> connected;
    for (const auto& pad : view.pads) if (pad.state != RowState::Absent) connected.push_back(pad.identity);
    return connected;
}
// The pad's own menu buttons (input::MenuButtons) select and go back here.
unsigned TargetInput(const PadView& pad) {
    unsigned result = 0;
    const std::pair<const char*, unsigned> buttons[] = {{"up", MenuInput::Up}, {"down", MenuInput::Down},
        {"left", MenuInput::Left}, {"right", MenuInput::Right}, {pad.menuButtons.confirmGlyph, MenuInput::Select}, {pad.menuButtons.backGlyph, MenuInput::Back}};
    for (const auto& button : buttons)
        if (std::find(pad.held.begin(), pad.held.end(), button.first) != pad.held.end()) result |= button.second;
    return result;
}
}
void ApplicationShell::SyncLayoutPaths(const ShellView& view) {
    if (view.padLayer != Layer::Active) return;
    const auto connected = ConnectedLayoutPads(view);
    for (const auto& pad : connected) {
        const auto match = FindLayout(preferences_.buttonLayouts, pad, connected);
        if (match.byGuid) preferences_.buttonLayouts[*match.index].identity.path = pad.path;
    }
}
void ApplicationShell::SetGameplayLayoutPrompts(const ShellView& view) {
    ButtonLayout table = DefaultLayout;
    for (const auto& pad : view.pads) if (pad.gameplay) {
        table = pad.table;
        if (preferencesDirty_) {
            const auto match = FindLayout(preferences_.buttonLayouts, pad.identity, ConnectedLayoutPads(view));
            if (match.index) table = preferences_.buttonLayouts[*match.index].buttons;
        }
    }
    SetMenuPromptBrand(ResolvePrompts(preferences_.buttonPrompts, view.inputDevice.type, view.gameplayBrand), NamesOf(table));
}
void ApplicationShell::CancelLayoutCapture(const ShellView& view, const Submit& submit) {
    if (!layoutCapture_.Active() || !layoutPad_) return;
    layoutCapture_.Read(ImGui::GetTime(), {}, layoutPad_->has, LayoutCancel::State);
    LayoutLog(view, submit, fmt::format("layout capture for id {} {} cancelled (state)", layoutPad_->id,
        layoutCaptureFunction_ ? "assign " + LayoutCaptureRow(layoutCaptureFunction_) : std::string("pick")));
    layoutNeutral_ = true; menu_.navigation.Rearm();
}
void ApplicationShell::OpenButtonLayout(const PadView& pad) {
    layoutPad_ = pad; layoutChanged_ = false; layoutAbsent_ = pad.state == RowState::Absent;
    layoutCapture_ = {}; layoutCaptureFunction_.reset(); layoutNeutral_ = true;
    menu_.navigation.Push("button-layout");
    menu_.navigation.Focus("test", ButtonLayoutRows(pad, true));
}
bool ApplicationShell::ControllerActivate(const MenuAction& action, const ShellView& view, const Submit& submit) {
    const auto& screen = menu_.navigation.Screen();
    if (screen == "player" && action.id == "controls") {
        const auto pad = std::find_if(view.pads.begin(), view.pads.end(), [](const PadView& pad) { return pad.gameplay; });
        if (pad != view.pads.end() && view.atMainMenu && !netplay::Fighting(view.session.match) && view.padLayer == Layer::Active && view.inputDevice.type != input::PadKeyboard) OpenButtonLayout(*pad);
        return true;
    }
    if (screen != "controllers" || action.id.compare(0, 4, "pad-") != 0) return false;
    const auto request = ControllerAction(action, view.pads);
    if (!request) return true;
    const auto pad = std::find_if(view.pads.begin(), view.pads.end(), [&](const PadView& pad) { return pad.id == request->id; });
    if (pad == view.pads.end()) return true;
    if (request->kind == ControllerRequest::Kind::OpenLayout) { if (view.atMainMenu && !netplay::Fighting(view.session.match)) OpenButtonLayout(*pad); }
    else if (request->kind == ControllerRequest::Kind::Setup) BeginPadSetup(*pad, view, submit);
    else {
        ShellAction sent; sent.command.generation = view.session.generation; sent.padAction = *request;
        if (!submit(std::move(sent))) error_ = loc::T("error.queue_failed");
    }
    return true;
}
bool ApplicationShell::SavePreferences(const ShellView& view, const Submit& submit) {
    if (view.canEditPreferences) return Send(netplay::CommandKind::SavePreferences, view, submit);
    ShellAction action; action.command.generation = view.session.generation;
    action.roomSettings = true; action.preferences = preferences_;
    return submit(std::move(action));
}
void ApplicationShell::LayoutLog(const ShellView& view, const Submit& submit, std::string text) {
    ShellAction action; action.command.generation = view.session.generation;
    action.padAction.kind = ControllerRequest::Kind::LayoutLog; action.padAction.log = std::move(text);
    submit(std::move(action));
}
bool ApplicationShell::ChangeLayout(const PadView& pad, const std::optional<ButtonLayout>& layout,
    const ShellView& view, const Submit& submit, const std::string& log, const std::optional<BoundButtons>& bound,
    const std::optional<ExtraCopies>& extras) {
    if (!CanEditLayout(view) || !Served(pad)) return false;
    const auto binding = bound.value_or(pad.bound);
    const auto copies = extras.value_or(pad.extras);
    ShellAction action; action.command.generation = view.session.generation;
    action.padAction.kind = layout ? ControllerRequest::Kind::Layout : ControllerRequest::Kind::RemoveLayout;
    action.padAction.id = pad.id; action.padAction.buttons = layout.value_or(DefaultLayout); action.padAction.bound = binding;
    action.padAction.extras = copies; action.padAction.log = log;
    if (!submit(std::move(action))) { error_ = loc::T("error.queue_failed"); return false; }
    if (layout) UpsertLayout(preferences_.buttonLayouts, {pad.identity, *layout, static_cast<std::int64_t>(std::time(nullptr)), binding, copies});
    else RemoveLayout(preferences_.buttonLayouts, pad.identity);
    preferencesDirty_ = true; saveAt_ = ImGui::GetTime() + .45; error_.clear();
    layoutChanged_ = layout.has_value();
    return true;
}
void ApplicationShell::ChangeBoundButton(const PadView& pad, BoundAction action, unsigned button, const ShellView& view, const Submit& submit) {
    const auto& row = BoundActionRows[action];
    layoutFlash_ = row.id; layoutFlashUntil_ = ImGui::GetTime() + .6;
    // Partners never share a button: the one the other acts on now is refused.
    const auto partner = BoundActions[action].partner;
    if (LayoutButtonMask(static_cast<int>(button), pad.has) == BoundButtonNow(pad, partner)) {
        LayoutLog(view, submit, fmt::format("layout for id {}: {} = {} refused (its partner's button)", pad.id, row.id, LayoutGlyphs[button]));
        notice_ = loc::Tf("button_layout.menu_taken", pad.buttonNames[button], loc::T(BoundActionRows[partner].label));
        noticeTone_ = Tone::Error; noticeUntil_ = ImGui::GetTime() + 4; layoutFlash_.clear();
        return;
    }
    auto bound = pad.bound;
    bound[action] = static_cast<int>(button);
    if (bound != pad.bound && ChangeLayout(pad, pad.table, view, submit,
        fmt::format("layout for id {}: {} = {}", pad.id, row.id, LayoutGlyphs[button]), bound)) {
        notice_ = loc::Tf("button_layout.assigned", loc::T(row.label), pad.buttonNames[button]); noticeTone_ = Tone::Success; noticeUntil_ = ImGui::GetTime() + 4;
    }
}
void ApplicationShell::ChangeExtraCopy(const PadView& pad, unsigned extra, const std::string& choice, const ShellView& view, const Submit& submit) {
    auto copies = pad.extras; copies[extra] = -1;
    for (int target = 0; target < CopyTargetCount; ++target) if (choice == CopyTargetGlyph(target)) copies[extra] = target;
    if (copies == pad.extras) return;
    const auto copy = CopiedButton(copies, extra, pad.has);
    if (!ChangeLayout(pad, pad.table, view, submit, fmt::format("layout for id {}: {} copies {}", pad.id, ExtraIds[extra], copy ? CopyTargetGlyph(static_cast<int>(*copy)) : "nothing"), {}, copies)) return;
    const auto name = ExtraButtonName(pad, extra);
    notice_ = copy ? loc::Tf("button_layout.extra_set", name, CopyTargetName(pad, static_cast<int>(*copy))) : loc::Tf("button_layout.extra_off", name);
    noticeTone_ = Tone::Success; noticeUntil_ = ImGui::GetTime() + 4;
}
bool ApplicationShell::DrawButtonLayout(const ShellView& view, const Submit& submit) {
    auto& nav = menu_.navigation;
    if (nav.Screen() != "button-layout" && nav.Screen() != "button-layout-button") {
        if (layoutPad_) {
            CancelLayoutCapture(view, submit);
            menu_.listEnabled = true; menu_.ownsBack = false; menu_.customDetail = false;
            menu_.additionalInput = 0; menu_.compactDetailHeight = 0; menu_.listCard = {};
            if (nav.Screen() != "pad-setup") layoutPad_.reset();
        }
        return false;
    }
    if (!layoutPad_) {
        const auto pad = std::find_if(view.pads.begin(), view.pads.end(), [](const PadView& pad) { return pad.gameplay; });
        if (pad == view.pads.end()) return false;
        layoutPad_ = *pad;
    }
    const auto found = std::find_if(view.pads.begin(), view.pads.end(), [&](const PadView& pad) { return Same(pad.identity, layoutPad_->identity); });
    auto pad = found == view.pads.end() ? *layoutPad_ : *found;
    if (found == view.pads.end()) { pad.state = RowState::Absent; pad.held.clear(); pad.picture = "controller_disconnected"; }
    const bool absent = pad.state == RowState::Absent;
    const bool editable = CanEditLayout(view) && Served(pad);
    const auto stored = FindLayout(preferences_.buttonLayouts, pad.identity, ConnectedLayoutPads(view));
    pad.layout = stored.index ? std::make_optional(preferences_.buttonLayouts[*stored.index].buttons) : std::nullopt;
    pad.bound = stored.index ? preferences_.buttonLayouts[*stored.index].bound : NoBoundButtons;
    pad.extras = stored.index ? preferences_.buttonLayouts[*stored.index].extras : NoExtraCopies;
    if (pad.layout) pad.table = *pad.layout;
    pad.buttonNames = LayoutButtonNames(pad, preferences_.buttonPrompts);
    if (absent) pad.held.clear();
    const bool wasAbsent = layoutAbsent_; layoutAbsent_ = absent;
    if (absent && !wasAbsent) LayoutLog(view, submit, fmt::format("layout editor: '{}' id {} disconnected; editing paused", pad.name, pad.id));
    *layoutPad_ = pad;
    if (nav.Screen() == "button-layout-button" && !editable) { nav.Return(); nav.Focus("edit-button", ButtonLayoutRows(pad, false)); }
    bool editor = nav.Screen() == "button-layout-button";
    // Capture waits for every button up, unlike the menu's gate: a held one would be bound.
    if (layoutNeutral_ && pad.held.empty()) layoutNeutral_ = false;
    SetGameplayLayoutPrompts(view);
    const bool capturing = layoutCapture_.Active();
    const auto boundCapture = BoundActionOfCapture(layoutCaptureFunction_);
    const std::optional<std::uint32_t> captureMask = layoutCaptureFunction_ && !boundCapture ? std::make_optional(LayoutFunctions[*layoutCaptureFunction_].mask) : std::nullopt;
    const auto captureRow = LayoutCaptureRow(layoutCaptureFunction_);
    const LayoutRowMarks marks{capturing ? captureRow : std::string(), ImGui::GetTime() < layoutFlashUntil_ ? layoutFlash_ : std::string()};
    auto rows = editor ? ButtonEditorRows(pad, layoutButton_, editable) : ButtonLayoutRows(pad,
        editable || ((pad.state == RowState::GameRead || pad.state == RowState::Unmapped) && view.canEditPreferences), marks);
    if (wasAbsent != absent) nav.Focus(absent ? "layout-absent" : "test", rows);
    menu_.listCard = {};
    if (capturing) {
        const auto left = layoutCapture_.Left(ImGui::GetTime());
        const auto card = boundCapture ? BoundCaptureCard(pad, *boundCapture, left) : LayoutCaptureCard(pad, captureMask, left);
        menu_.listCard = {[card](float width) { return CaptureCardHeight(card, width); }, [card] { DrawCaptureCard(card); }};
    }
    menu_.listEnabled = !capturing; menu_.ownsBack = true; menu_.customDetail = capturing;
    menu_.additionalInput = !capturing && !layoutNeutral_ && !pad.gameplay ? TargetInput(pad) : 0;
    menu_.shortcutHints = capturing ? std::vector<LegendHint>{{"Start", loc::T("common.cancel")}, {"Esc", loc::T("common.cancel")}} : std::vector<LegendHint>{};
    if (capturing) SetMenuPromptBrand(ResolvePrompts(preferences_.buttonPrompts, input::PadXInput, pad.brand), NamesOf(pad.table));
    menu_.backHint = capturing ? loc::T("common.cancel") : "";
    menu_.footerNote.clear(); menu_.typesText = false; menu_.wideListShare = .46f; menu_.compactDetailHeight = 360;
    const auto status = saveFailed_ ? loc::T("common.save_failed") : preferencesDirty_ || saveQueued_ || view.settingsPending ? loc::T("common.saving") : loc::T("common.saved");
    const bool notice = !notice_.empty() && ImGui::GetTime() < noticeUntil_;
    const auto message = !error_.empty() ? error_ : notice ? notice_ + "\n" + status : std::string(status);
    const auto action = menu_.Draw(loc::T(editor ? "button_layout.button_title" : "button_layout.title"), rows,
        message.c_str(), [&](const std::string&) {
            if (editor) DrawButtonEditorDetail(pad, preferences_.buttonPrompts, layoutButton_);
            else DrawButtonLayoutDetail(pad, preferences_.buttonPrompts, capturing);
        }, 1, {}, {}, 0, 100, false, !error_.empty() || saveFailed_ ? Tone::Error : notice ? noticeTone_ : preferencesDirty_ || saveQueued_ || view.settingsPending ? Tone::Pending : Tone::Neutral);
    AnswerDialog(submit);
    if (capturing) {
        const auto cancel = !editable ? LayoutCancel::State : ImGui::IsKeyPressed(ImGuiKey_Escape, false) ? LayoutCancel::Escape : action.kind == MenuAction::Back ? LayoutCancel::Back : LayoutCancel::None;
        const auto press = layoutCapture_.Read(ImGui::GetTime(), pad.held, pad.has, cancel);
        const auto captureName = layoutCaptureFunction_ ? "assign " + captureRow : std::string("pick");
        if (press.cancelled != LayoutCancel::None) {
            LayoutLog(view, submit, fmt::format("layout capture for id {} {} cancelled ({})", pad.id, captureName, CancelName(press.cancelled)));
            nav.Rearm(); layoutNeutral_ = true;
        } else if (press.button) {
            const auto button = *press.button;
            LayoutLog(view, submit, fmt::format("layout capture for id {} {} took {}", pad.id, captureName, LayoutGlyphs[button]));
            layoutNeutral_ = true; nav.Rearm();
            if (boundCapture) ChangeBoundButton(pad, *boundCapture, button, view, submit);
            else if (!captureMask) { layoutButton_ = button; nav.Push("button-layout-button"); nav.Focus("bit-lp", ButtonEditorRows(pad, button, true)); }
            else {
                layoutFlash_ = captureRow; layoutFlashUntil_ = ImGui::GetTime() + .6;
                auto layout = pad.table; const auto assigned = Assign(layout, button, *captureMask, pad.has);
                if (assigned.changed && ChangeLayout(pad, layout, view, submit,
                    fmt::format("layout for id {}: {} = {} ({})", pad.id, LayoutGlyphs[button], FightName(*captureMask), nav.Focus()))) {
                    const auto name = AttackLongName(*captureMask);
                    if (assigned.swapped) {
                        const auto other = *assigned.swapped;
                        LayoutLog(view, submit, fmt::format("layout for id {}: {} = {} (swap)", pad.id, LayoutGlyphs[other], FightName(assigned.old)));
                        notice_ = assigned.old ? loc::Tf("button_layout.swapped", name, pad.buttonNames[button], pad.buttonNames[other], FightName(assigned.old)) :
                            loc::Tf("button_layout.swapped_nothing", name, pad.buttonNames[button], pad.buttonNames[other]);
                    } else notice_ = loc::Tf("button_layout.assigned", name, pad.buttonNames[button]);
                    noticeTone_ = Tone::Success; noticeUntil_ = ImGui::GetTime() + 4;
                }
            }
        } else if (const auto extra = UncopiedExtraHeld(pad)) {
            // An extra button that copies nothing has no press to give; say how to give it one.
            notice_ = loc::Tf("button_layout.extra_uncopied", ExtraButtonName(pad, *extra)); noticeTone_ = Tone::Error; noticeUntil_ = ImGui::GetTime() + 4;
        }
        return true;
    }
    if (action.kind == MenuAction::Back) {
        if (editor) { nav.Return(); nav.Focus("edit-button", ButtonLayoutRows(pad, editable)); }
        else {
            const auto missing = MissingAttacks(pad.table, pad.has);
            if (layoutChanged_ && !missing.empty()) {
                std::string names, shortNames;
                for (auto mask : missing) { if (!names.empty()) { names += ", "; shortNames += ", "; } names += AttackLongName(mask); shortNames += FightName(mask); }
                dialogContext_.generation = view.session.generation;
                dialogContext_.log = fmt::format("layout for id {} left with no button for {}", pad.id, shortNames);
                menu_.ShowNotice(NoticeDialog{loc::Tf("button_layout.missing_text", names), loc::T("button_layout.missing_heading"),
                    {{loc::T("button_layout.keep_editing")}, {loc::T("button_layout.leave"), NoticeResult::LeaveButtonLayout}}});
            } else nav.Return();
        }
    } else if (action.kind == MenuAction::Activate && action.id == "setup") BeginPadSetup(pad, view, submit);
    else if (editable && action.kind == MenuAction::Activate && (action.id.compare(0, 3, "fn-") == 0 || BoundActionOfRow(action.id) || action.id == "edit-button")) {
        layoutCaptureFunction_.reset();
        for (unsigned i = 0; i < LayoutFunctions.size(); ++i) if (action.id == std::string("fn-") + LayoutFunctions[i].id) layoutCaptureFunction_ = i;
        if (const auto bound = BoundActionOfRow(action.id)) layoutCaptureFunction_ = BoundActionCapture(*bound);
        layoutCapture_.Begin(ImGui::GetTime(), pad.held);
    } else if (editable && action.kind == MenuAction::Chosen && action.id == "preset") {
        for (const auto& preset : LayoutPresets) if (action.text == preset.id && ChangeLayout(pad, preset.buttons, view, submit,
            fmt::format("layout for id {} set to preset {}", pad.id, preset.id))) {
            notice_ = loc::Tf("button_layout.preset_applied", loc::T(preset.key)); noticeTone_ = Tone::Neutral; noticeUntil_ = ImGui::GetTime() + 4;
        }
    } else if (editable && action.kind == MenuAction::Chosen && ExtraOfRow(action.id)) {
        ChangeExtraCopy(pad, *ExtraOfRow(action.id), action.text, view, submit);
    } else if (editable && action.kind == MenuAction::Activate && action.id == "training-clear") {
        auto bound = pad.bound; bound[TrainingRecordButton] = bound[TrainingPlayButton] = -1;
        if (ChangeLayout(pad, pad.table, view, submit, fmt::format("layout for id {}: Training buttons cleared", pad.id), bound)) {
            nav.Focus("training-record", rows); notice_ = loc::T("button_layout.training_cleared"); noticeTone_ = Tone::Neutral; noticeUntil_ = ImGui::GetTime() + 4;
        }
    } else if (editable && action.kind == MenuAction::Activate && action.id == "remove") {
        if (ChangeLayout(pad, {}, view, submit, fmt::format("layout for id {} removed", pad.id))) {
            nav.Focus("preset", rows); notice_ = loc::T("button_layout.removed"); noticeTone_ = Tone::Neutral; noticeUntil_ = ImGui::GetTime() + 4;
        }
    } else if (editable && editor) {
        auto layout = pad.table;
        if (action.kind == MenuAction::Adjust) for (unsigned i = 0; i < 6; ++i) if (action.id == std::string("bit-") + LayoutFunctions[i].id)
            SetMask(layout, layoutButton_, layout[layoutButton_] ^ AttackMasks[i]);
        if (action.kind == MenuAction::Activate && action.id == "clear") { SetMask(layout, layoutButton_, 0); nav.Focus("bit-lp", rows); }
        if (layout != pad.table) ChangeLayout(pad, layout, view, submit,
            fmt::format("layout for id {}: {} = {} (by hand)", pad.id, LayoutGlyphs[layoutButton_], FightName(layout[layoutButton_])));
    }
    return true;
}
} }
