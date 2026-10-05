#pragma once

#include "../common/InputDelay.hxx"

#include <cstdint>
#include <string>

namespace sf4e { namespace netplay {

// Auto's connection check against the seated opponent. It wants one check per
// opponent, shortly after they sit down, and keeps what that check measured:
// a table's revision moves whenever anyone readies, queues or watches, so the
// recommendation is held for as long as the opponent is the same.
//
// Ready holds for the check, but never for long: HoldMs after the opponent
// sits down, or after Auto is chosen, it goes ahead with what was measured or
// with two frames. A spectator or a queued member has no opponent, so nothing
// is checked or held for them.
class AutoDelayCheck {
public:
    // The table settles for this long before the check is asked for, counted
    // from the seating and from any later move of the table's revision. The
    // opponent's helper checks the pair at that revision against its own copy
    // of the room, and refuses a reservation it has not caught up with.
    static constexpr std::uint64_t SettleMs = 1000;
    // A check the room cannot take yet is wanted for this long past that: a
    // seat still settling, a recovering room, or a queued opponent whose
    // spectator link from the last game is still being released. It ends
    // with the hold.
    static constexpr std::uint64_t GiveUpMs = 11000;
    // A check that ends without a recommendation is asked for once more after
    // this. Two fighters on Auto ask at the same moment, and the room commits
    // one reservation at a time.
    static constexpr std::uint64_t RetryMs = 1500;
    static constexpr int Attempts = 2;
    // Ready never holds for the check longer than this. A tournament start or
    // a slow relay must not wait on a measurement.
    static constexpr std::uint64_t HoldMs = 12000;
    // An opponent gone for less than this keeps what was measured against
    // them: a room recovering, or a seat left and taken back, is the same
    // connection. Longer, or a different opponent, is measured again.
    static constexpr std::uint64_t ReturnMs = 30000;

    // The seated opponent and the table's revision, or no opponent.
    void Seat(const std::string& opponent, std::uint64_t pairRevision, std::uint64_t nextRequest, std::uint64_t nowMs) {
        if (opponent.empty()) {
            if (present_) { present_ = false; leftAtMs_ = nowMs; }
            return;
        }
        if (opponent != opponent_ || (!present_ && nowMs - leftAtMs_ >= ReturnMs)) {
            opponent_ = opponent;
            recommended_ = -1;
            firstRequest_ = nextRequest;
            asked_ = false;
            present_ = true;
            revision_ = pairRevision;
            settledFromMs_ = nowMs;
            Restart(nowMs);
            return;
        }
        // Back within ReturnMs: measured stays measured, and a check still
        // under way for them still counts. One not measured yet waits again.
        if (!present_) {
            present_ = true;
            revision_ = pairRevision;
            settledFromMs_ = nowMs;
            if (recommended_ < 0 && !asked_) Restart(nowMs);
            return;
        }
        if (pairRevision != revision_) { revision_ = pairRevision; settledFromMs_ = nowMs; }
    }
    // Auto was chosen: an opponent not measured yet gets a check. One already
    // under way carries on.
    void Want(std::uint64_t nowMs) {
        if (!asked_) Restart(nowMs);
    }
    // The room's connection check as it stands. A result made against this
    // opponent since they sat down counts, whoever asked for it. Auto's own
    // check that ends without a recommendation, or never started, is asked
    // for again while attempts remain.
    void Observe(const std::string& peer, std::uint64_t request, const std::string& status, int recommended, std::uint64_t nowMs) {
        if (!opponent_.empty() && peer == opponent_ && request >= firstRequest_ && recommended >= 0)
            recommended_ = recommended;
        if (asked_ && (request != askedRequest_ || peer != opponent_ || status != "checking")) {
            asked_ = false;
            if (recommended_ < 0) askAtMs_ = nowMs + RetryMs;
        }
    }
    // Auto asked for a check under this request number.
    void Asked(std::uint64_t request) { asked_ = true; askedRequest_ = request; ++attempts_; }

    bool Measured(const std::string& opponent) const { return recommended_ >= 0 && !opponent.empty() && opponent == opponent_; }
    // The check is still to be asked for.
    bool Wanted(std::uint64_t nowMs) const {
        return present_ && recommended_ < 0 && !asked_ && attempts_ < Attempts &&
            nowMs < askAtMs_ + GiveUpMs && nowMs < holdUntilMs_;
    }
    bool Due(std::uint64_t nowMs) const { return Wanted(nowMs) && nowMs >= askAtMs_ && nowMs >= settledFromMs_ + SettleMs; }
    // Ready holds: a check is under way or still to come, and the hold has
    // not run out.
    bool Holding(std::uint64_t nowMs) const {
        return present_ && recommended_ < 0 && nowMs < holdUntilMs_ && (asked_ || Wanted(nowMs));
    }
    // The delay Auto readies with against `opponent`.
    int Delay(const std::string& opponent) const { return AutoInputDelay(Measured(opponent) ? recommended_ : -1); }

private:
    void Restart(std::uint64_t nowMs) {
        attempts_ = 0;
        askAtMs_ = nowMs + SettleMs;
        holdUntilMs_ = nowMs + HoldMs;
    }

    std::string opponent_;
    bool present_ = false;
    int recommended_ = -1;
    std::uint64_t firstRequest_ = 1;
    std::uint64_t revision_ = 0;
    std::uint64_t settledFromMs_ = 0;
    std::uint64_t leftAtMs_ = 0;
    std::uint64_t askAtMs_ = 0;
    std::uint64_t holdUntilMs_ = 0;
    std::uint64_t askedRequest_ = 0;
    int attempts_ = 0;
    bool asked_ = false;
};

} }
