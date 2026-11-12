#pragma once
#include <cstdint>

namespace sf4e {

// An attack press made while GGPO refuses the local input.
//
// When GGPO refuses a frame's local input (most often PREDICTION_THRESHOLD,
// the remote side too far behind), the frame does not advance and the sample
// read for it is never queued. The next accepted frame sends whatever is held
// then, so a tap that starts and ends inside the refusal reached neither PC.
// This latch keeps such a tap and adds it to the next accepted sample, for
// that one frame, so the game sees the press it missed.
//
// It carries a press only when doing so cannot make a move the player did not
// input:
// - one attack button only. Two taps inside one stall would land on the same
//   frame, which the game reads as a chord (LP+LK is a throw, MP+MK a Focus
//   Attack, HP+HK a taunt), so then nothing is carried;
// - the stick where it was when the button went down. A press kept for a
//   later direction is another move (crouching MK becomes forward MK), so a
//   moved stick drops it;
// - never a direction: a latched direction could complete a motion.
// A button held in the last sample sent makes no new press, so it is not kept.
//
// Only what this PC sends changes, and GGPO delivers it to both sides, so the
// two simulations still agree.
class StallPresses {
public:
    // The game's six attack bits, in both the mapped and the raw word:
    // LP MP LK MK = 0x10-0x80, HP HK = 0x400 and 0x800 (training::AttackButtons).
    static constexpr std::uint32_t AttackBits = 0xcf0;
    static constexpr std::uint32_t DirectionBits = 0xf;

    struct Word { std::uint32_t mapped = 0, raw = 0; };

    // The sample to hand GGPO this frame.
    Word Send(Word sampled) const {
        if (!Carries(sampled)) return sampled;
        return {sampled.mapped | pending_.mapped, sampled.raw | pending_.raw};
    }

    // GGPO refused the frame whose sample this was.
    void Refused(Word sampled) {
        if (dropped_) return;
        const Word pressed{sampled.mapped & AttackBits & ~sent_.mapped, sampled.raw & AttackBits & ~sent_.raw};
        const std::uint32_t added = pressed.mapped & ~pending_.mapped;
        if (!added) return;
        if (pending_.mapped || (added & (added - 1))) {
            // A second button: carrying both would be a chord.
            pending_ = {};
            dropped_ = true;
            return;
        }
        pending_ = pressed;
        stick_ = sampled.mapped & DirectionBits;
    }

    // GGPO accepted `sent` (what Send returned for `sampled`). Returns 1 when
    // the latch carried a press made and let go inside the refusal, else 0.
    unsigned Accepted(Word sampled, Word sent) {
        const unsigned carried = Carries(sampled) && (pending_.mapped & ~sampled.mapped) ? 1u : 0u;
        pending_ = {};
        dropped_ = false;
        sent_ = sent;
        return carried;
    }

    void Reset() { *this = {}; }

private:
    bool Carries(Word sampled) const {
        return pending_.mapped && (sampled.mapped & DirectionBits) == stick_;
    }

    Word pending_, sent_;
    std::uint32_t stick_ = 0;
    // Two presses inside this refusal: carry none until a frame is accepted.
    bool dropped_ = false;
};

} // namespace sf4e
