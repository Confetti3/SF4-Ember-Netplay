#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>

namespace sf4e {
// One entry per state restore, incremented only after a replayed frame succeeds,
// and the session's totals, which the runtime logs as the match ends.
// This is presentation telemetry, never part of a rollback save state.
class RollbackHud {
public:
    // The session's restores, the frames replayed after them, and the most
    // replayed after one.
    struct Totals {
        std::uint32_t rollbacks = 0, frames = 0, deepest = 0;
    };
private:
    struct Burst { std::uint64_t time; unsigned frames; };
    std::deque<Burst> bursts_;
    Totals totals_;
public:
    void Reset() { bursts_.clear(); totals_ = Totals(); }
    void Begin(std::uint64_t now) { Expire(now); bursts_.push_back({now, 0}); ++totals_.rollbacks; }
    void Replayed(std::uint64_t now) {
        if (bursts_.empty()) return;
        Burst& burst = bursts_.back();
        ++burst.frames; burst.time = now;
        ++totals_.frames;
        totals_.deepest = (std::max)(totals_.deepest, static_cast<std::uint32_t>(burst.frames));
    }
    const Totals& Session() const { return totals_; }
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
