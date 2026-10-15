#pragma once
#include <cstdint>

namespace sf4e { namespace netplay {

// When the runtime publishes its snapshot. Publishing deep-copies the room,
// the preferences and the member list, and samples the pad for Ember's menu.
// Where that pad drives a menu (outside a fight, or over a Practice match this
// PC fights in) every tick is published, so no press is missed between two
// and none is held longer than it was. In any other fight only the match HUD
// and the input fault check read the snapshot, so a tick is published only
// when an input to it changed (its fingerprint), or every BackstopTicks for
// anything the fingerprint does not cover.
class PublishCadence {
public:
    static constexpr unsigned BackstopTicks = 6;
    // fingerprint is asked only while publication is throttled.
    template<class Fingerprint>
    bool Due(bool everyTick, const Fingerprint& fingerprint) {
        if (everyTick) { fingerprint_ = 0; ticks_ = 0; return true; }
        const std::uint64_t now = fingerprint();
        if (now == fingerprint_ && ++ticks_ < BackstopTicks) return false;
        fingerprint_ = now; ticks_ = 0;
        return true;
    }
private:
    std::uint64_t fingerprint_ = 0;
    unsigned ticks_ = 0;
};

} }
