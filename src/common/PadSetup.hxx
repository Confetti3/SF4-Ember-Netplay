#pragma once
#include "PadSetupInput.hxx"
#include "SdlMapping.hxx"
#include <array>
#include <string_view>
#include <utility>
#include <vector>

namespace sf4e { namespace gamepads {
// A stick step maps the whole axis its push lands on (5.3).
enum class StepKind { Direction, Button, Stick };
struct SetupStep { const char* name; const char* nameKey; const char* target; bool required; StepKind kind; };
inline constexpr std::array<SetupStep, 20> SetupSteps = {{
    {"up", "pad_setup.step.up", "dpup", true, StepKind::Direction}, {"down", "pad_setup.step.down", "dpdown", true, StepKind::Direction},
    {"left", "pad_setup.step.left", "dpleft", true, StepKind::Direction}, {"right", "pad_setup.step.right", "dpright", true, StepKind::Direction},
    {"lp", "pad_setup.step.lp", "x", true, StepKind::Button}, {"mp", "pad_setup.step.mp", "y", true, StepKind::Button},
    {"hp", "pad_setup.step.hp", "rightshoulder", true, StepKind::Button}, {"lk", "pad_setup.step.lk", "a", true, StepKind::Button},
    {"mk", "pad_setup.step.mk", "b", true, StepKind::Button}, {"hk", "pad_setup.step.hk", "righttrigger", true, StepKind::Button},
    {"3p", "pad_setup.step.3p", "leftshoulder", false, StepKind::Button}, {"3k", "pad_setup.step.3k", "lefttrigger", false, StepKind::Button},
    {"ls", "pad_setup.step.ls", "leftstick", false, StepKind::Button}, {"rs", "pad_setup.step.rs", "rightstick", false, StepKind::Button},
    {"lx", "pad_setup.step.lx", "leftx", false, StepKind::Stick}, {"ly", "pad_setup.step.ly", "lefty", false, StepKind::Stick},
    {"rx", "pad_setup.step.rx", "rightx", false, StepKind::Stick}, {"ry", "pad_setup.step.ry", "righty", false, StepKind::Stick},
    {"start", "pad_setup.step.start", "start", true, StepKind::Button}, {"select", "pad_setup.step.select", "back", false, StepKind::Button}
}};
inline constexpr int SetupStepCount = static_cast<int>(SetupSteps.size());
// Steps read by position: summary navigation and the tap ladder's back and skip.
inline constexpr int UpStep = 0, DownStep = 1, LeftStep = 2, RightStep = 3, LkStep = 7, MkStep = 8;
static_assert(std::string_view(SetupSteps[UpStep].target) == "dpup" && std::string_view(SetupSteps[DownStep].target) == "dpdown" &&
    std::string_view(SetupSteps[LeftStep].target) == "dpleft" && std::string_view(SetupSteps[RightStep].target) == "dpright" &&
    std::string_view(SetupSteps[LkStep].target) == "a" && std::string_view(SetupSteps[MkStep].target) == "b", "named steps moved");
// Whether a mapping string binds every target the wizard requires. SDL's built-in
// database has entries that leave some out (HJC QanBa Joystick Plus: no triggers),
// and such a pad is not mapped until Ember's setup records the rest. A field that
// is not "target:input" (GUID, name, hints) and a target with no input is skipped.
inline bool MapsRequiredSteps(std::string_view mapping) {
    std::vector<std::string_view> bound;
    while (!mapping.empty()) {
        const auto field = mapping.substr(0, mapping.find(','));
        mapping.remove_prefix((std::min)(mapping.size(), field.size() + 1));
        const auto colon = field.find(':');
        if (colon == std::string_view::npos || colon + 1 == field.size()) continue;
        auto target = field.substr(0, colon);
        if (!target.empty() && (target.front() == '+' || target.front() == '-')) target.remove_prefix(1);
        bound.push_back(target);
    }
    return std::all_of(SetupSteps.begin(), SetupSteps.end(), [&](const SetupStep& step) {
        return !step.required || std::find(bound.begin(), bound.end(), step.target) != bound.end();
    });
}
using SetupInputs = std::array<std::optional<RawInput>, SetupSteps.size()>;
// A raw input as SDL binds it (RawBinding's text): a half axis from 0, a
// trigger axis whole and inverted when it rests high, a hat by its direction bit.
inline SdlInput BindingInput(const RawInput& input) {
    switch (input.kind) {
    case RawKind::Button: return {SdlBindType::Button, input.index};
    case RawKind::AxisPlus: return {SdlBindType::Axis, input.index, 0, SdlAxisMax};
    case RawKind::AxisMinus: return {SdlBindType::Axis, input.index, 0, SdlAxisMin};
    case RawKind::Trigger: return {SdlBindType::Axis, input.index, input.inverted ? SdlAxisMax : SdlAxisMin, input.inverted ? SdlAxisMin : SdlAxisMax};
    default: return {SdlBindType::Hat, input.index, 0, 0, 1 << (static_cast<int>(input.kind) - 1)};
    }
}
// A step's binding. A stick step maps the full axis: a push to plus reads the
// axis as it is, a push to minus inverted, and a button, hat or trigger drives the plus half.
inline SdlBinding SetupBinding(int step, RawInput input) {
    const auto& setup = SetupSteps[step];
    auto output = SdlTarget(setup.target);
    if (setup.kind == StepKind::Stick) {
        if (input.kind == RawKind::AxisPlus || input.kind == RawKind::AxisMinus)
            input = {RawKind::Trigger, input.index, false, input.kind == RawKind::AxisMinus};
        else output.min = 0;
    }
    return {BindingInput(input), output};
}
// The same output, either half of an axis: a stick step takes its whole axis.
inline bool SameTarget(const SdlOutput& a, const SdlOutput& b) { return a.type == b.type && a.index == b.index; }
// Whether one physical movement can drive both: the same button, hat masks that
// share a direction, or axis ranges on the same side of 0 (a full axis covers both).
inline bool Overlaps(const SdlInput& a, const SdlInput& b) {
    if (a.type != b.type || a.index != b.index) return false;
    switch (a.type) {
    case SdlBindType::Hat: return (a.hatMask & b.hatMask) != 0;
    case SdlBindType::Axis: return (std::min)(a.min, a.max) < (std::max)(b.min, b.max) && (std::min)(b.min, b.max) < (std::max)(a.min, a.max);
    default: return true;
    }
}
enum class SetupMode { Capture, Summary };
enum class SetupFeedback { None, Conflict, Required, Idle, Reconnect };
enum class SetupCommand { None, Escape, Backspace, Space, Redo, Restart, Cancel, Save, LeftMenu, Activity };
enum class SetupEnd { None, Hold, Escape, Confirm, Disconnect, Idle, LeftMenu, Saved, SaveFailed };
inline const char* SetupEndName(SetupEnd end) {
    switch (end) {
    case SetupEnd::Hold: return "hold";
    case SetupEnd::Escape: return "escape";
    case SetupEnd::Confirm: return "confirm";
    case SetupEnd::Disconnect: return "disconnect";
    case SetupEnd::Idle: return "idle";
    case SetupEnd::LeftMenu: return "left menu";
    case SetupEnd::Saved: return "saved";
    case SetupEnd::SaveFailed: return "save failed";
    default: return "";
    }
}
enum class SetupEventKind { Record, Back, Skip, Conflict, Required, Cancel, Save, Stopped, Ignored };
struct SetupEvent {
    SetupEventKind kind;
    int step = 0, other = -1;
    std::optional<RawInput> input;
    SetupEnd reason = SetupEnd::None;
};
enum class SetupMenuAction { None, Select, Back };
struct SetupSummaryInput {
    bool up = false, down = false;
    SetupMenuAction action = SetupMenuAction::None;
    std::uint64_t sequence = 0;
};
// The summary's draft menu bindings: the newly recorded Up, Down, LK and MK,
// never the installed table's buttons (5.8). The summary's prompts name these
// same inputs, so the advertised button is the one that acts before Save.
struct SetupMenuDraft {
    std::optional<RawInput> up, down, select, back;
};
inline SetupMenuDraft DraftMenu(const SetupInputs& recorded) {
    return {recorded[UpStep], recorded[DownStep], recorded[LkStep], recorded[MkStep]};
}
// Summary selection uses the same tap ladder, so holding LK on Save can
// still cancel. Latched actions survive until the drawing thread sees them.
class SetupSummaryControls {
public:
    SetupSummaryInput Read(std::uint64_t now, const SetupMenuDraft& draft, const std::vector<RawInput>& held) {
        const auto pressed = [&](const std::optional<RawInput>& input) { return input && std::find(held.begin(), held.end(), *input) != held.end(); };
        state_.up = pressed(draft.up); state_.down = pressed(draft.down);
        const std::optional<RawInput>* menuInputs[] = {&draft.select, &draft.back};
        for (int i = 0; i < 2; ++i) {
            if (pressed(*menuInputs[i])) { if (!since_[i]) since_[i] = now; }
            else if (since_[i]) {
                if (now - *since_[i] < 1000) { state_.action = i == 0 ? SetupMenuAction::Select : SetupMenuAction::Back; ++state_.sequence; }
                since_[i].reset();
            }
        }
        return state_;
    }
    void Release() { since_ = {}; state_.up = state_.down = false; }
private:
    SetupSummaryInput state_;
    std::array<std::optional<std::uint64_t>, 2> since_;
};
struct SetupState {
    bool active = false, releaseButtons = false;
    SetupMode mode = SetupMode::Capture;
    int step = 0, conflictStep = -1;
    SetupInputs recorded;
    std::array<bool, SetupSteps.size()> skipped{};
    std::vector<RawInput> held;
    std::uint64_t holdMs = 0;
    SetupFeedback feedback = SetupFeedback::None;
    std::optional<RawInput> conflictInput;
    int reconnectSeconds = 0;
    SetupSummaryInput summaryInput;
    // Filled in summary mode only.
    SetupMenuDraft menuDraft;
    bool CanSave() const {
        for (unsigned i = 0; i < SetupSteps.size(); ++i) if (SetupSteps[i].required && !recorded[i]) return false;
        return true;
    }
};
class PadSetup {
public:
    void Start(std::uint64_t now, const std::optional<SetupAxisEntry>& entry = {}) { *this = {}; reader_.Reset(entry); state_.active = true; lastInput_ = now; }
    const SetupState& State() const { return state_; }
    const std::vector<int>& Rest() const { return reader_.Rest(); }
    std::vector<SetupEvent> Update(std::uint64_t now, const RawState& raw, bool connected = true,
        SetupCommand command = SetupCommand::None, int step = 0) {
        events_.clear();
        if (!state_.active) return events_;
        if (command != SetupCommand::None) lastInput_ = now;
        if (command == SetupCommand::LeftMenu) Stop(SetupEventKind::Cancel, SetupEnd::LeftMenu);
        else if (command == SetupCommand::Cancel || (command == SetupCommand::Escape && state_.mode == SetupMode::Capture))
            Stop(SetupEventKind::Cancel, command == SetupCommand::Escape ? SetupEnd::Escape : SetupEnd::Confirm);
        if (!state_.active) return events_;
        if (!connected) {
            if (!disconnected_) { disconnected_ = now; gesture_ = Gesture::Ready; state_.holdMs = 0; state_.held.clear(); state_.summaryInput.up = state_.summaryInput.down = false; summaryControls_.Release(); }
            if (now - *disconnected_ >= 15000) Stop(SetupEventKind::Stopped, SetupEnd::Disconnect);
            else { state_.feedback = SetupFeedback::Reconnect; state_.reconnectSeconds = static_cast<int>((15000 - (now - *disconnected_) + 999) / 1000); }
            return events_;
        }
        if (disconnected_) {
            if (now - *disconnected_ >= 15000) { Stop(SetupEventKind::Stopped, SetupEnd::Disconnect); return events_; }
            disconnected_.reset(); reader_.Reset(); lastInput_ = now; state_.feedback = SetupFeedback::None;
        }
        if (command == SetupCommand::Restart) {
            state_ = {}; state_.active = true; redo_ = false; lastInput_ = now; feedbackUntil_ = 0;
            gesture_ = Gesture::WaitForRest; summaryControls_.Release();
        }
        if (command == SetupCommand::Redo && state_.mode == SetupMode::Summary && step >= 0 && step < SetupStepCount) {
            state_.step = step; Clear(step); state_.mode = SetupMode::Capture; redo_ = true; gesture_ = Gesture::WaitForRest; state_.holdMs = 0;
        }
        if (command == SetupCommand::Save && state_.mode == SetupMode::Summary && state_.CanSave())
            events_.push_back({SetupEventKind::Save});
        if (state_.mode == SetupMode::Capture) {
            if (command == SetupCommand::Backspace || command == SetupCommand::Space) {
                gesture_ = Gesture::WaitForRest; state_.holdMs = 0;
            }
            if (command == SetupCommand::Backspace) Back();
            if (command == SetupCommand::Space) Skip(now);
        }
        const auto reading = reader_.Read(now, raw);
        state_.releaseButtons = !reading.ready; state_.held = reading.held;
        for (const auto& input : reading.ignored) events_.push_back({SetupEventKind::Ignored, state_.step, -1, input});
        if (!reading.atRest || command != SetupCommand::None) lastInput_ = now;
        if (reading.owner && gesture_ == Gesture::Ready) { gesture_ = Gesture::Holding; pressedAt_ = now; }
        if (gesture_ == Gesture::Holding) {
            const auto elapsed = now - pressedAt_;
            state_.holdMs = elapsed;
            if (elapsed >= 3000) { Stop(SetupEventKind::Cancel, SetupEnd::Hold); return events_; }
            if (std::find(reading.held.begin(), reading.held.end(), *reading.owner) == reading.held.end()) {
                gesture_ = elapsed < 1000 && state_.mode == SetupMode::Capture ? Gesture::TapPending : Gesture::WaitForRest;
                state_.holdMs = 0;
            }
        }
        if (reading.atRest) {
            if (gesture_ == Gesture::TapPending && reading.owner) Tap(*reading.owner, now);
            gesture_ = Gesture::Ready;
        }
        if (state_.mode == SetupMode::Summary) {
            state_.menuDraft = DraftMenu(state_.recorded);
            state_.summaryInput = summaryControls_.Read(now, state_.menuDraft, reading.held);
        } else { state_.menuDraft = {}; summaryControls_.Release(); state_.summaryInput.up = state_.summaryInput.down = false; }
        if (now - lastInput_ >= 60000) Stop(SetupEventKind::Stopped, SetupEnd::Idle);
        else if (now >= feedbackUntil_) state_.feedback = state_.mode == SetupMode::Capture && now - lastInput_ >= 10000 ? SetupFeedback::Idle : SetupFeedback::None;
        return events_;
    }
private:
    void Clear(int step) { state_.recorded[step].reset(); state_.skipped[step] = false; }
    void Stop(SetupEventKind kind, SetupEnd reason) { state_.active = false; state_.summaryInput = {}; events_.push_back({kind, state_.step, -1, {}, reason}); }
    void Back() {
        if (!state_.step) return;
        --state_.step; Clear(state_.step); redo_ = false;
        events_.push_back({SetupEventKind::Back, state_.step});
    }
    void Advance() {
        if (redo_) { redo_ = false; state_.mode = SetupMode::Summary; }
        else if (++state_.step == SetupStepCount) { state_.step = SetupStepCount - 1; state_.mode = SetupMode::Summary; }
    }
    void Skip(std::uint64_t now) {
        if (SetupSteps[state_.step].required) {
            state_.feedback = SetupFeedback::Required; feedbackUntil_ = now + 2000;
            events_.push_back({SetupEventKind::Required, state_.step});
        } else { state_.recorded[state_.step].reset(); state_.skipped[state_.step] = true; events_.push_back({SetupEventKind::Skip, state_.step}); Advance(); }
    }
    void Tap(const RawInput& input, std::uint64_t now) {
        if (state_.step > RightStep && state_.recorded[LeftStep] && input == *state_.recorded[LeftStep]) { Back(); return; }
        if (state_.step > RightStep && state_.recorded[RightStep] && input == *state_.recorded[RightStep]) { Skip(now); return; }
        // A stick step's full axis also conflicts with either half recorded elsewhere.
        for (int i = 0; i < SetupStepCount; ++i) if (i != state_.step && state_.recorded[i] &&
            Overlaps(SetupBinding(state_.step, input).input, SetupBinding(i, *state_.recorded[i]).input)) {
            state_.feedback = SetupFeedback::Conflict; state_.conflictStep = i; state_.conflictInput = input; feedbackUntil_ = now + 2000;
            events_.push_back({SetupEventKind::Conflict, state_.step, i, input}); return;
        }
        state_.recorded[state_.step] = input; state_.skipped[state_.step] = false;
        state_.feedback = SetupFeedback::None; feedbackUntil_ = 0;
        events_.push_back({SetupEventKind::Record, state_.step, -1, input}); Advance();
    }
    SetupState state_;
    SetupInputReader reader_;
    SetupSummaryControls summaryControls_;
    std::vector<SetupEvent> events_;
    // The reader owns the input. The model only tracks the gesture's phase.
    enum class Gesture { Ready, Holding, TapPending, WaitForRest };
    Gesture gesture_ = Gesture::Ready;
    std::optional<std::uint64_t> disconnected_;
    std::uint64_t lastInput_ = 0, pressedAt_ = 0, feedbackUntil_ = 0;
    bool redo_ = false;
};
// Over the bindings SDL makes for the pad now (its own mapping or an earlier
// Ember setup, as SDL_GetGamepadBindings gives them), recorded steps take their
// targets and the inputs they read: a binding to the same target, or from an
// input one of them reads, goes. Every other binding stays, in SDL's order, so
// a skipped step keeps the pad's own binding (5.9).
inline std::vector<SdlBinding> MergeSetup(const SetupState& state, const std::vector<SdlBinding>& existing) {
    std::vector<SdlBinding> merged;
    for (int i = 0; i < SetupStepCount; ++i) if (state.recorded[i]) merged.push_back(SetupBinding(i, *state.recorded[i]));
    const auto recorded = merged.size();
    for (const auto& binding : existing)
        if (std::none_of(merged.begin(), merged.begin() + recorded, [&](const SdlBinding& step) {
            return SameTarget(step.output, binding.output) || Overlaps(step.input, binding.input);
        })) merged.push_back(binding);
    return merged;
}
// The mapping a save installs: the recorded steps over SDL's snapshot of the
// pad, which keeps the rest of the snapshot. None when SDL could not be read,
// since a failed read is not a pad that binds nothing, or when SDL cannot be
// given the merge.
inline std::optional<std::string> SetupMapping(const SetupState& state, std::optional<SdlSnapshot> pad, const std::string& guid,
    const std::string& name, std::string_view existing) {
    if (!pad) return {};
    pad->bindings = MergeSetup(state, pad->bindings);
    return WriteSdlMapping(guid, name, std::move(*pad), existing);
}
} }
