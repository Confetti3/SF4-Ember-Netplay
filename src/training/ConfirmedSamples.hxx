#pragma once
#include "FrameMeter.hxx"
#include "../common/ConfirmedCheckpoint.hxx"
#include <array>

// The fighters of a rollback match, a frame at a time, for a reader that must
// never see a frame that is later played again with other inputs: the frame
// meter. Every simulated frame is captured under its GGPO save frame, a
// resimulated one over its earlier capture, as native_result::Timeline keeps
// outcomes; a frame is given out, once and in order, when every input in it
// is confirmed.
namespace sf4e { namespace training {
class ConfirmedSamples {
public:
    // More than GGPO predicts ahead, so a frame is confirmed before its slot is used again.
    static constexpr int Capacity = 64;
    void Reset() { slots_ = {}; given_ = 0; }
    void Capture(int stateFrame, const std::array<FighterSample, 2>& fighters) {
        if (stateFrame <= 0) return;
        // A given frame is confirmed and is not played again: the count began anew.
        if (stateFrame <= given_) Reset();
        auto& slot = slots_[stateFrame % Capacity];
        slot.frame = stateFrame; slot.fighters = fighters;
    }
    // The next confirmed frame after the last one given. After a hole (frames
    // nobody captured) it goes on from the oldest frame held; the frame number
    // tells the reader that frames are missing.
    bool Next(int lastConfirmedInput, int& frame, std::array<FighterSample, 2>& fighters) {
        int next = given_ + 1;
        if (slots_[next % Capacity].frame != next) {
            next = 0;
            for (const auto& slot : slots_) if (slot.frame > given_ && (!next || slot.frame < next)) next = slot.frame;
        }
        if (!statehash::IsConfirmedCheckpoint(next, lastConfirmedInput)) return false;
        frame = given_ = next; fighters = slots_[next % Capacity].fighters;
        return true;
    }
private:
    struct Slot { int frame = 0; std::array<FighterSample, 2> fighters; };
    std::array<Slot, Capacity> slots_{};
    int given_ = 0;
};
} }
