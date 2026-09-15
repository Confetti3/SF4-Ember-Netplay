#pragma once
#include <cstdint>
namespace sf4e {
class MatchTelemetry {
    bool polled_ = false;
    std::uint64_t lastPoll_ = 0, lastSample_ = 0;
    int ping_ = -1;
public:
    int appliedDelay = -1;
    bool spectator = false;
    void Reset(bool watching = false) { *this = {}; spectator = watching; }
    bool PollDue(std::uint64_t now) {
        if (spectator || (polled_ && now - lastPoll_ < 250)) return false;
        polled_ = true; lastPoll_ = now; return true;
    }
    void Sample(std::uint64_t now, int ping) { ping_ = ping; lastSample_ = now; }
    int Ping(std::uint64_t now) const { return spectator || ping_ < 0 || now - lastSample_ > 2000 ? -1 : ping_; }
    void AppliedDelay(int frames, bool success) { appliedDelay = success ? frames : -1; }
};
}
