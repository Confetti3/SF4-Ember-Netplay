#pragma once
#include <cstdint>

namespace sf4e {

// Attack presses made while GGPO refuses the local input.
//
// When the remote side falls too far behind, ggpo_add_local_input returns
// PREDICTION_THRESHOLD and the frame does not advance: the sample read for it
// is never queued. The next accepted frame sends whatever is held then, so a
// tap that starts and ends inside the stall reached neither PC. This latch
// keeps the attack buttons first pressed during refused frames and adds them
// to the next accepted sample, for that one frame, if they were not already
// held in the last sample sent (only then does the game see a new press).
//
// Directions are left out on purpose: a latched direction could complete a
// motion the player never made. A button held before the stall, let go and
// pressed again inside it, still loses that second press: one frame cannot
// carry a release and a press.
//
// Only what this PC sends changes, and GGPO delivers it to both sides, so the
// two simulations still agree.
class StallPresses {
public:
    // The game's six attack bits, in both the mapped and the raw word:
    // LP MP LK MK = 0x10-0x80, HP HK = 0x400 and 0x800 (training::AttackButtons).
    static constexpr std::uint32_t AttackBits = 0xcf0;

    struct Word { std::uint32_t mapped = 0, raw = 0; };

    // The sample to hand GGPO this frame.
    Word Send(Word sampled) const {
        return {sampled.mapped | (pending_.mapped & ~sent_.mapped), sampled.raw | (pending_.raw & ~sent_.raw)};
    }

    // GGPO refused the frame whose sample this was.
    void Refused(Word sampled) {
        pending_.mapped |= sampled.mapped & AttackBits & ~sent_.mapped;
        pending_.raw |= sampled.raw & AttackBits & ~sent_.raw;
    }

    // GGPO accepted `sent` (what Send returned for `sampled`). Returns how many
    // presses were made and let go inside the stall, which the latch carried;
    // the fight reads the mapped word, so they are counted there.
    unsigned Accepted(Word sampled, Word sent) {
        unsigned carried = 0;
        for (std::uint32_t bits = pending_.mapped & ~sampled.mapped & ~sent_.mapped; bits; bits &= bits - 1) ++carried;
        pending_ = {};
        sent_ = sent;
        return carried;
    }

    void Reset() { *this = {}; }

private:
    Word pending_, sent_;
};

} // namespace sf4e
