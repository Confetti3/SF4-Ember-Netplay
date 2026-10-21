// The pad's chord, Back then Start, by the dummy's mode: on standby it is Record
// and opens nothing, so a pad alone can record; at rest, while
// recording and while playing it opens the controls, which interrupts a
// recording and keeps it (TrainingSession.hxx: ChordStartsRecording,
// TrainingPad.hxx: TrainingPadInput).
#include "../common/TrainingPad.hxx"
#include "../training/TrainingSession.hxx"
#include "test_support.hxx"
#include <cstdint>
#include <cstdio>

using namespace sf4e;
// One chord and its release, as the pad poll sees it, for a mode; what it
// asked for over the three samples.
struct Chord {
    input::TrainingPadInput pad;
    input::TrainingControls controls;
    double now = 0;
    input::TrainingPadEvents Press(training::Mode mode) {
        const float place[2] = {0, 0};
        input::PadOwner owner; owner.generation = 1; owner.epoch = controls.Epoch();
        input::TrainingPadEvents seen;
        for (const std::uint32_t physical : {input::PhysicalBack, input::PhysicalBack | input::PhysicalStart, 0u}) {
            now += .05;
            const auto events = pad.Update(physical, controls, now, input::TrainingCall::None, owner, place,
                training::ChordStartsRecording(mode)).events;
            seen.open = seen.open || events.open; seen.close = seen.close || events.close;
            seen.record = seen.record || events.record;
            seen.reset = seen.reset || events.reset; seen.save = seen.save || events.save;
            seen.owned |= events.owned;
        }
        return seen;
    }
};

int main() {
    using training::Mode;
    // Standby: the recording starts; the controls stay shut, the Start is
    // still the chord's so the game never pauses on it, and Back neither
    // resets nor saves.
    {
        Chord chord;
        const auto seen = chord.Press(Mode::Standby);
        CHECK(seen.record && !seen.open && !chord.controls.Open());
        CHECK((seen.owned & input::PhysicalStart) != 0 && !seen.reset && !seen.save);
    }
    // At rest, recording and playing: the chord opens the controls, as ever.
    for (const Mode mode : {Mode::Idle, Mode::Recording, Mode::Playback}) {
        Chord chord;
        const auto seen = chord.Press(mode);
        CHECK(seen.open && !seen.record && chord.controls.Open() && !seen.reset && !seen.save);
    }
    // The player's own Record and Play buttons (the stick clicks): a fresh
    // press, alone, with the controls shut and the fight holding the pad.
    {
        const input::TrainingButtons buttons{input::xinput::LS, input::xinput::RS};
        input::TrainingButtonPresses presses;
        const auto press = [&](std::uint32_t physical, bool open = false, input::TrainingCall call = input::TrainingCall::None, bool live = true) {
            return presses.Update(physical, buttons, open, call, live);
        };
        CHECK(press(input::xinput::LS).record && !press(0).record);
        // Held, it does not repeat; let go and pressed again, it does.
        CHECK(press(input::xinput::RS).play && !press(input::xinput::RS).play && !press(0).play);
        CHECK(press(input::xinput::LS | input::xinput::RS).record && !press(0).play);
        // Neither with Back held, nor while the controls are open, a room calls, or the pad is not the fight's.
        CHECK(!press(input::PhysicalBack | input::xinput::LS).record && !press(0).record);
        CHECK(!press(input::xinput::LS, true).record && !press(0).record);
        CHECK(!press(input::xinput::LS, false, input::TrainingCall::Called).record && !press(0).record);
        CHECK(!press(input::xinput::LS, false, input::TrainingCall::None, false).record && !press(0).record);
        // A button held through any of that fires on its next press, not when it ends.
        CHECK(!press(input::xinput::LS, true).record && !press(input::xinput::LS).record && !press(0).record && press(input::xinput::LS).record);
        // Reset drops a press under way: held across it, nothing fires until it is pressed again.
        presses.Reset();
        CHECK(!press(input::xinput::LS).record && !press(0).record && press(input::xinput::LS).record && !press(0).record);
        // No binding, no presses: the stick click does nothing in Training.
        input::TrainingButtonPresses bare;
        CHECK(!bare.Update(input::xinput::LS, {}, false, input::TrainingCall::None, true).record);
    }
    // Record and Play are posted as the position events are, under the owner
    // the press went down with, so they reach the controls' queue in order.
    {
        input::TrainingControls queue;
        for (const auto kind : {input::TrainingPadEvent::Kind::Record, input::TrainingPadEvent::Kind::Play}) {
            input::TrainingPadEvent event; event.kind = kind; event.generation = 1; event.epoch = queue.Epoch();
            CHECK(queue.Post(event));
        }
        const auto taken = queue.Take();
        CHECK(taken.size() == 2 && taken[0].kind == input::TrainingPadEvent::Kind::Record && taken[1].kind == input::TrainingPadEvent::Kind::Play);
    }
    std::puts("Training pad standby checks passed.");
    return 0;
}
