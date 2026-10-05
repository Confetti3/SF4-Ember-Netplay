#pragma once

#include "../common/InputDelay.hxx"

#include <cstdint>
#include <string>

namespace sf4e { namespace netplay {

// Auto's connection check against the seated opponent. It wants one check per
// opponent, shortly after they sit down, and keeps what that check measured:
// a table's revision moves whenever anyone readies, queues or watches, so the
// recommendation is held for as long as the opponent is the same.
class AutoDelayCheck {
public:
    // The table settles for this long before the check is asked for.
    static constexpr std::uint64_t SettleMs = 1000;
    // A check the room cannot take is wanted for this long past that.
    static constexpr std::uint64_t GiveUpMs = 4000;

    // The seated opponent, or none. A different one starts over.
    void Seat(const std::string& opponent, std::uint64_t nextRequest, std::uint64_t nowMs) {
        if (opponent == opponent_) return;
        opponent_ = opponent;
        recommended_ = -1;
        firstRequest_ = nextRequest;
        Want(nowMs);
    }
    // Auto was chosen: an opponent not measured yet gets a check.
    void Want(std::uint64_t nowMs) {
        asked_ = false;
        askAtMs_ = nowMs + SettleMs;
    }
    // A check's result. Only one made against this opponent since they sat
    // down counts, whoever asked for it.
    void Observe(const std::string& peer, std::uint64_t request, int recommended) {
        if (!opponent_.empty() && peer == opponent_ && request >= firstRequest_ && recommended >= 0)
            recommended_ = recommended;
    }
    void Asked() { asked_ = true; }

    const std::string& Opponent() const { return opponent_; }
    bool Measured(const std::string& opponent) const { return recommended_ >= 0 && opponent == opponent_; }
    // The check is still to be asked for.
    bool Wanted(std::uint64_t nowMs) const {
        return !opponent_.empty() && !asked_ && recommended_ < 0 && nowMs < askAtMs_ + GiveUpMs;
    }
    bool Due(std::uint64_t nowMs) const { return Wanted(nowMs) && nowMs >= askAtMs_; }
    // The delay Auto readies with against `opponent`.
    int Delay(const std::string& opponent) const { return AutoInputDelay(Measured(opponent) ? recommended_ : -1); }

private:
    std::string opponent_;
    int recommended_ = -1;
    std::uint64_t firstRequest_ = 1;
    std::uint64_t askAtMs_ = 0;
    bool asked_ = false;
};

} }
