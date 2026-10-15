#pragma once

// The GGPO state frame of the state the battle engine holds, and the order a
// frame on GGPO's inputs is simulated, sampled and saved in. The played frame
// (PlayGgpoFrame), a rollback's re-simulated frames
// (ggpo_advance_frame_callback) and GGPO's save and load callbacks all go
// through it, so the frame identity and order a desync trace relies on are the
// ones its test drives (frame_trace_test.cxx).

#include "ConfirmedCheckpoint.hxx"

namespace sf4e { namespace statehash {

class GgpoTimeline {
public:
    // -1 until GGPO saves frame 0, which it does before the first input.
    int Held() const { return held_; }
    // The state frame the next simulated frame reaches.
    int Next() const { return StateFrameAfterInput(held_); }
    void Reset() { held_ = -1; }

    // GGPO's save callback, after each simulated frame.
    void Saved(int frame) { held_ = frame; }

    // GGPO's load callback, as a rollback starts: restore() brings the engine
    // back to the saved frame, and the re-simulated frames are named from it.
    template <class Restore>
    bool Load(int frame, Restore restore) {
        if (!restore()) return false;
        held_ = frame;
        return true;
    }

    // One frame on GGPO's inputs: run() simulates it, sample() records it,
    // then advance() lets GGPO save it. That advance may poll and roll back
    // over this very frame, re-simulating it here with the real inputs, so
    // the sample is taken first and the re-simulated one replaces it.
    template <class Run, class Sample, class Advance>
    static auto Simulate(Run run, Sample sample, Advance advance) -> decltype(advance()) {
        run();
        sample();
        return advance();
    }

private:
    int held_ = -1;
};

} }
