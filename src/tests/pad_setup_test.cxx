#include "../common/PadSetup.hxx"
#include <iostream>
#include <stdexcept>
#include "test_support.hxx"
using namespace sf4e::gamepads;
struct Wizard {
    PadSetup model;
    RawState raw;
    std::uint64_t now = 0;
    explicit Wizard(int axes = 0) { raw.buttons.resize(SetupStepCount); raw.axes.assign(axes, 0); model.Start(now); Tick(); }
    std::vector<SetupEvent> Tick(std::uint64_t elapsed = 0, SetupCommand command = SetupCommand::None, int step = 0, bool connected = true) {
        now += elapsed; return model.Update(now, raw, connected, command, step);
    }
    std::vector<SetupEvent> Tap(int button, std::uint64_t duration = 100) {
        raw.buttons[button] = true; Tick(); raw.buttons[button] = false; return Tick(duration);
    }
    std::vector<SetupEvent> Push(int axis, int value) {
        raw.axes[axis] = value; Tick(); raw.axes[axis] = 0; return Tick(100);
    }
    void Through(int step) { while (model.State().mode == SetupMode::Capture && model.State().step < step) Tap(model.State().step); }
};
void Gestures() {
    for (const auto duration : {0, 999, 1000, 1500, 2999}) {
        Wizard w; auto e = w.Tap(0, duration);
        CHECK_OR_THROW(w.model.State().active); CHECK_OR_THROW(w.model.State().step == (duration < 1000 ? 1 : 0));
        CHECK_OR_THROW(e.empty() == (duration >= 1000)); CHECK_OR_THROW(w.model.State().holdMs == 0);
    }
    Wizard hold; hold.raw.buttons[0] = true; hold.Tick(); hold.Tick(1000);
    CHECK_OR_THROW(hold.model.State().holdMs == 1000 && !hold.model.State().recorded[0]);
    auto cancelled = hold.Tick(2000); CHECK_OR_THROW(!hold.model.State().active && cancelled[0].reason == SetupEnd::Hold);
    Wizard back; back.Through(4); back.Tap(2);
    CHECK_OR_THROW(back.model.State().step == 3 && !back.model.State().recorded[3]);
    Wizard required; required.Through(4); required.Tap(3);
    CHECK_OR_THROW(required.model.State().step == 4 && required.model.State().feedback == SetupFeedback::Required);
    required.Tick(1999); CHECK_OR_THROW(required.model.State().feedback == SetupFeedback::Required);
    required.Tick(1); CHECK_OR_THROW(required.model.State().feedback == SetupFeedback::None);
    Wizard skip; skip.Through(10); skip.Tap(3); CHECK_OR_THROW(skip.model.State().step == 11 && skip.model.State().skipped[10]);
    Wizard conflict; conflict.Through(4); auto e = conflict.Tap(0);
    CHECK_OR_THROW(e[0].kind == SetupEventKind::Conflict && conflict.model.State().conflictStep == 0);
    conflict.Tick(2000); CHECK_OR_THROW(conflict.model.State().feedback == SetupFeedback::None);
    Wizard ownership; ownership.raw.buttons[0] = true; ownership.Tick();
    ownership.raw.buttons[1] = true; ownership.Tick(50);
    ownership.raw.buttons[0] = false; ownership.Tick(50); ownership.Tick(3000);
    CHECK_OR_THROW(ownership.model.State().active && ownership.model.State().step == 0);
    ownership.raw.buttons[1] = false; ownership.Tick(); CHECK_OR_THROW(ownership.model.State().step == 1 && ownership.model.State().recorded[0]->index == 0);
    Wizard directions; directions.Tap(0); directions.Tap(0); CHECK_OR_THROW(directions.model.State().step == 1);
}
void SummaryAndKeyboard() {
    for (int step = 0; step < SetupStepCount; ++step) {
        Wizard required; required.Through(step); const auto before = required.model.State().step;
        const auto events = required.Tick(0, SetupCommand::Space);
        CHECK_OR_THROW(events[0].kind == (SetupSteps[step].required ? SetupEventKind::Required : SetupEventKind::Skip));
        if (SetupSteps[step].required) CHECK_OR_THROW(required.model.State().step == before);
    }
    Wizard w; w.Tick(0, SetupCommand::Backspace); CHECK_OR_THROW(w.model.State().step == 0);
    w.Tick(0, SetupCommand::Space); CHECK_OR_THROW(w.model.State().feedback == SetupFeedback::Required);
    w.Tap(0); w.Tick(0, SetupCommand::Backspace); CHECK_OR_THROW(!w.model.State().recorded[0]);
    w.Through(10); for (int step = 10; step < SetupStepCount - 2; ++step) w.Tick(0, SetupCommand::Space);
    w.Tap(SetupStepCount - 2); w.Tick(0, SetupCommand::Space);
    CHECK_OR_THROW(w.model.State().mode == SetupMode::Summary && w.model.State().CanSave());
    CHECK_OR_THROW(w.Tick(0, SetupCommand::Save)[0].kind == SetupEventKind::Save);
    w.Tick(0, SetupCommand::Redo, 7); CHECK_OR_THROW(!w.model.State().recorded[7] && !w.model.State().CanSave());
    w.Tap(16); CHECK_OR_THROW(w.model.State().mode == SetupMode::Summary && w.model.State().step == 7);
    w.raw.buttons[16] = true; w.Tick(); CHECK_OR_THROW(w.model.State().summaryInput.sequence == 0);
    w.raw.buttons[16] = false; w.Tick(50);
    CHECK_OR_THROW(w.model.State().summaryInput.action == SetupMenuAction::Select && w.model.State().summaryInput.sequence == 1);
    w.Tick(0, SetupCommand::Redo, 10); w.Tick(0, SetupCommand::Space); CHECK_OR_THROW(w.model.State().mode == SetupMode::Summary);
    w.Tick(0, SetupCommand::Restart); CHECK_OR_THROW(w.model.State().mode == SetupMode::Capture && w.model.State().step == 0);
    for (const auto& input : w.model.State().recorded) CHECK_OR_THROW(!input);
    CHECK_OR_THROW(!w.model.State().CanSave());
    w.Tick(0, SetupCommand::Escape); CHECK_OR_THROW(!w.model.State().active);
    Wizard summaryHold; summaryHold.Through(SetupStepCount - 1); summaryHold.Tap(SetupStepCount - 1);
    summaryHold.raw.buttons[7] = true; summaryHold.Tick(); summaryHold.Tick(1000);
    CHECK_OR_THROW(summaryHold.model.State().summaryInput.sequence == 0);
    CHECK_OR_THROW(summaryHold.Tick(2000)[0].reason == SetupEnd::Hold);
    Wizard chord; chord.Through(SetupStepCount - 1); chord.Tap(SetupStepCount - 1); chord.raw.buttons[0] = true; chord.Tick();
    chord.raw.buttons[7] = true; chord.Tick(50); chord.raw.buttons[7] = false; chord.Tick(50);
    CHECK_OR_THROW(chord.model.State().summaryInput.up && chord.model.State().summaryInput.action == SetupMenuAction::Select);
    Wizard entry; entry.model.Start(entry.now, SetupAxisEntry{0, -32768}); entry.raw.axes = {32767}; entry.Tick();
    CHECK_OR_THROW(entry.model.State().releaseButtons && !entry.model.State().recorded[0]);
    entry.raw.axes[0] = -32768; entry.Tick(100); CHECK_OR_THROW(!entry.model.State().recorded[0]);
    entry.raw.axes[0] = 32767; entry.Tick(); entry.raw.axes[0] = -32768; entry.Tick(100);
    CHECK_OR_THROW(entry.model.State().recorded[0] && RawBinding(*entry.model.State().recorded[0]) == "a0");
    Wizard confirm; CHECK_OR_THROW(confirm.Tick(0, SetupCommand::Cancel)[0].reason == SetupEnd::Confirm);
    Wizard silent; CHECK_OR_THROW(silent.Tick(0, SetupCommand::LeftMenu)[0].reason == SetupEnd::LeftMenu);
}
void Timers() {
    Wizard idle; idle.Tick(9999); CHECK_OR_THROW(idle.model.State().feedback == SetupFeedback::None);
    idle.Tick(1); CHECK_OR_THROW(idle.model.State().feedback == SetupFeedback::Idle);
    idle.Tick(49999); CHECK_OR_THROW(idle.model.State().active); CHECK_OR_THROW(idle.Tick(1)[0].reason == SetupEnd::Idle);
    Wizard keys; keys.Tick(59000, SetupCommand::Activity); keys.Tick(59000); CHECK_OR_THROW(keys.model.State().active);
    Wizard disconnect; disconnect.Through(7); disconnect.Tick(0, SetupCommand::None, 0, false);
    CHECK_OR_THROW(disconnect.model.State().reconnectSeconds == 15);
    disconnect.Tick(14999, SetupCommand::None, 0, false); CHECK_OR_THROW(disconnect.model.State().reconnectSeconds == 1);
    disconnect.Tick(); CHECK_OR_THROW(disconnect.model.State().active && disconnect.model.State().step == 7);
    disconnect.Tap(7); CHECK_OR_THROW(disconnect.model.State().step == 8);
    disconnect.Tick(0, SetupCommand::None, 0, false);
    CHECK_OR_THROW(disconnect.Tick(15000, SetupCommand::None, 0, false)[0].reason == SetupEnd::Disconnect);
    Wizard deadline; deadline.Tick(0, SetupCommand::None, 0, false); CHECK_OR_THROW(deadline.Tick(15000)[0].reason == SetupEnd::Disconnect);
    Wizard summary; summary.Through(SetupStepCount - 1); summary.Tap(SetupStepCount - 1); summary.Tick(0, SetupCommand::None, 0, false);
    summary.Tick(14000); CHECK_OR_THROW(summary.model.State().mode == SetupMode::Summary);
    summary.raw.buttons[7] = true; summary.Tick(); summary.raw.buttons[7] = false; summary.Tick(100);
    const auto sequence = summary.model.State().summaryInput.sequence;
    summary.Tick(0, SetupCommand::None, 0, false); summary.Tick(14000);
    CHECK_OR_THROW(summary.model.State().summaryInput.sequence == sequence);
}
int Step(const char* name) {
    for (int i = 0; i < SetupStepCount; ++i) if (std::string(SetupSteps[i].name) == name) return i;
    throw std::runtime_error(name);
}
void Sticks() {
    CHECK_OR_THROW(SetupStepCount == 20 && Step("3k") == 11 && Step("ls") == 12 && Step("rs") == 13 && Step("start") == SetupStepCount - 2);
    CHECK_OR_THROW(SetupSteps[Step("lx")].kind == StepKind::Stick && !SetupSteps[Step("ry")].required && SetupSteps[Step("ls")].kind == StepKind::Button);
    // Up on the stick's minus half: the down stick step maps the whole axis, so it conflicts.
    Wizard w(2); w.Push(1, -32768); w.Tap(1); w.Tap(2); w.Tap(3); w.Through(Step("ly"));
    auto e = w.Push(1, 32767);
    CHECK_OR_THROW(e.size() == 1 && e[0].kind == SetupEventKind::Conflict && e[0].other == UpStep && w.model.State().step == Step("ly"));
    w.Push(0, 32767); CHECK_OR_THROW(w.model.State().step == Step("rx") && RawBinding(*w.model.State().recorded[Step("ly")]) == "+a0");
    // A half recorded after a stick step conflicts too (redo from the summary).
    w.Through(SetupStepCount - 1); w.Tap(SetupStepCount - 1); w.Tick(0, SetupCommand::Redo, UpStep);
    e = w.Push(0, -32768); CHECK_OR_THROW(e[0].kind == SetupEventKind::Conflict && e[0].other == Step("ly"));
    // The tap ladder still runs first: Left steps back and Right skips.
    Wizard ladder(1); ladder.Tap(0); ladder.Tap(1); ladder.Push(0, -32768); ladder.Push(0, 32767);
    ladder.Through(Step("lx"));
    e = ladder.Push(0, 32767); CHECK_OR_THROW(e[0].kind == SetupEventKind::Skip && ladder.model.State().step == Step("ly"));
    e = ladder.Push(0, -32768); CHECK_OR_THROW(e[0].kind == SetupEventKind::Back && ladder.model.State().step == Step("lx"));
}
void Reading() {
    SetupInputReader reader; RawState raw; raw.buttons.resize(8); raw.hats = {0}; raw.axes = {0, -32768, 32767};
    CHECK_OR_THROW(reader.Read(0, raw).atRest);
    raw.buttons[2] = true; auto hitbox = reader.Read(10, raw); CHECK_OR_THROW(hitbox.owner->kind == RawKind::Button);
    raw.hats[0] = 1; raw.axes[0] = -32768; CHECK_OR_THROW(reader.Read(20, raw).owner == hitbox.owner);
    raw.buttons[2] = false; raw.hats[0] = 0; raw.axes[0] = 0; CHECK_OR_THROW(reader.Read(30, raw).atRest);
    raw.hats[0] = 3; CHECK_OR_THROW(!reader.Read(40, raw).owner); raw.hats[0] = 1;
    auto hat = reader.Read(50, raw); CHECK_OR_THROW(RawBinding(*hat.owner) == "h0.1");
    raw.axes[0] = -32768; CHECK_OR_THROW(reader.Read(60, raw).owner == hat.owner);
    raw.hats[0] = 0; raw.axes[0] = 0; reader.Read(70, raw);
    raw.axes[1] = 32767; CHECK_OR_THROW(RawBinding(*reader.Read(80, raw).owner) == "a1");
    raw.axes[1] = -32768; reader.Read(90, raw);
    raw.axes[2] = -32768; CHECK_OR_THROW(RawBinding(*reader.Read(100, raw).owner) == "a2~");
    raw.axes[2] = 32767; reader.Read(110, raw);
    raw.axes[0] = 20000; CHECK_OR_THROW(RawBinding(*reader.Read(120, raw).owner) == "+a0");
    raw.axes[0] = 9000; CHECK_OR_THROW(!reader.Read(130, raw).atRest);
    raw.axes[0] = 8000; CHECK_OR_THROW(reader.Read(140, raw).atRest);
    raw.axes[0] = 20000; reader.Read(150, raw); raw.axes[0] = -20000;
    CHECK_OR_THROW(reader.Read(160, raw).held.size() == 1); raw.axes[0] = 0; reader.Read(170, raw);
    reader.Reset(); raw.buttons[0] = true;
    CHECK_OR_THROW(reader.Read(0, raw).releaseButtons); CHECK_OR_THROW(reader.Read(4999, raw).releaseButtons);
    auto stuck = reader.Read(5000, raw); CHECK_OR_THROW(stuck.ignored.size() == 1 && stuck.atRest);
    raw.buttons[0] = false; reader.Read(6000, raw); raw.buttons[0] = true; CHECK_OR_THROW(reader.Read(6100, raw).atRest);
    reader.Reset(); raw.buttons[0] = false; raw.hats = {0, 0}; reader.Read(0, raw);
    raw.axes[0] = -20000; const auto axisFirst = reader.Read(10, raw).owner;
    raw.hats[0] = 1; CHECK_OR_THROW(reader.Read(20, raw).owner == axisFirst);
    raw.axes[0] = 0; raw.hats[0] = 0; reader.Read(30, raw); raw.hats[1] = 8;
    CHECK_OR_THROW(reader.Read(40, raw).owner->numberedHat);
    reader.Reset(); raw = {}; raw.axes = {32767}; raw.rest = {-32768};
    CHECK_OR_THROW(reader.Read(0, raw).ready && reader.Rest()[0] == 32767);
    raw.axes[0] = -32768; CHECK_OR_THROW(RawBinding(*reader.Read(100, raw).owner) == "a0~");
}
int main() { Gestures(); SummaryAndKeyboard(); Timers(); Sticks(); Reading(); std::cout << "Pad setup gestures, state transitions, timers and raw inputs passed\n"; }
