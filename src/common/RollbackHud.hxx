#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>

namespace sf4e {
// One entry per state restore, incremented only after a replayed frame succeeds.
// This is presentation telemetry, never part of a rollback save state.
class RollbackHud {
    struct Burst { std::uint64_t time; unsigned frames; };
    std::deque<Burst> bursts_;
public:
    void Reset() { bursts_.clear(); }
    void Begin(std::uint64_t now) { Expire(now); bursts_.push_back({now, 0}); }
    void Replayed(std::uint64_t now) {
        if (!bursts_.empty()) { ++bursts_.back().frames; bursts_.back().time = now; }
    }
    unsigned Recent(std::uint64_t now) {
        Expire(now);
        unsigned depth = 0;
        for (const auto& burst : bursts_) depth = (std::max)(depth, burst.frames);
        return depth;
    }
private:
    void Expire(std::uint64_t now) {
        while (!bursts_.empty() && now - bursts_.front().time >= 1000) bursts_.pop_front();
    }
};
}
