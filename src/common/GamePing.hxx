#pragma once
#include <cstdint>

namespace sf4e {
// The round trips GGPO reported while a game was played, counted per whole
// millisecond so that a game of any length takes the same space. Auto reads
// the median and the 95th percentile from it at the end of the game
// (AutoDelayCheck::Played).
class GamePing {
public:
    // Pings above this count as this: the delay bands stop long before.
    static constexpr int MaximumMs = 1000;
    // A game that reported fewer pings says too little about the link to move
    // Auto: ten seconds of the four polls a second MatchTelemetry takes.
    static constexpr std::uint32_t MinimumSamples = 40;

    struct Summary {
        std::uint32_t samples;
        int medianMs, p95Ms;
    };

    void Add(int pingMs) {
        int ms = pingMs;
        if (ms < 0) ms = 0;
        if (ms > MaximumMs) ms = MaximumMs;
        ++counts_[ms];
        ++samples_;
    }

    // The same indexes as the connection check's displayed median and p95
    // (recovery.rs: summarize_probe), so a played game and a check agree on
    // what they measured.
    Summary Summarize() const {
        return Summary{samples_, samples_ ? Percentile(50) : 0, samples_ ? Percentile(95) : 0};
    }

private:
    // The ping at sorted position ceil((samples - 1) * percent / 100).
    int Percentile(std::uint32_t percent) const {
        const std::uint32_t position = ((samples_ - 1) * percent + 99) / 100;
        std::uint32_t seen = 0;
        for (int ms = 0; ms <= MaximumMs; ++ms) {
            seen += counts_[ms];
            if (seen > position) return ms;
        }
        return MaximumMs;
    }

    std::uint32_t counts_[MaximumMs + 1] = {};
    std::uint32_t samples_ = 0;
};
}
