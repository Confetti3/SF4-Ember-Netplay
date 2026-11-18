#pragma once

#include "../common/GamePing.hxx"
#include "../common/InputDelay.hxx"

#include <cstdint>
#include <string>

namespace sf4e { namespace netplay {

// One game just played, as the match left it: who the opponent was (the peer
// identity the match's grant linked, kept after the link closed), the match's
// generation, and its ping. It travels from the end of the match to the tick
// that consumes it, by which time the table may have seated someone else.
struct PlayedGame {
    std::uint64_t generation = 0;
    std::string opponent;
    GamePing::Summary ping = {};
};

// What Auto's recommendation against the seated opponent stands on: nothing
// measured yet, the round trip a connection check measured, or the ping of a
// game played against them.
struct AutoDelayBasis {
    enum class Source { None, Check, Game };
    Source source;
    RoundTrip rtt;
};

// Auto's connection check against the seated opponent. It wants one check per
// opponent, shortly after they sit down, and keeps what that check measured:
// a table's revision moves whenever anyone readies, queues or watches, so the
// recommendation is held for as long as the opponent is the same.
//
// Ready holds for the check, but never for long: HoldMs after the opponent
// sits down, or after Auto is chosen, it goes ahead with what was measured or
// with two frames. A spectator or a queued member has no opponent, so nothing
// is checked or held for them.
//
// What it measured becomes a recommendation through the game's own bands.
class AutoDelayCheck {
public:
    explicit AutoDelayCheck(InputDelayBands bands) : bands_(bands) {}

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
            basis_ = Unmeasured();
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
            if (!HasBasis() && !asked_) Restart(nowMs);
            return;
        }
        if (pairRevision != revision_) { revision_ = pairRevision; settledFromMs_ = nowMs; }
    }
    // Auto was chosen: an opponent not measured yet gets a check. One already
    // under way carries on.
    void Want(std::uint64_t nowMs) {
        if (!asked_) Restart(nowMs);
    }
    // The room's connection check as it stands, with what it measured (a
    // Check basis) or nothing. A result made against this opponent since they
    // sat down counts, whoever asked for it. Auto's own check that ends
    // without a measurement, or never started, is asked for again while
    // attempts remain.
    // Each result counts once, when it first shows: the room keeps the last
    // one, and re-reading it every tick would undo what a later game measured.
    // Returns whether this one counted.
    bool Observe(const std::string& peer, std::uint64_t request, const std::string& status, const AutoDelayBasis& result, std::uint64_t nowMs) {
        const bool counts = !opponent_.empty() && peer == opponent_ && request >= firstRequest_ && request > checkedRequest_ &&
            result.source == AutoDelayBasis::Source::Check;
        if (counts) {
            basis_ = result;
            checkedRequest_ = request;
        }
        if (asked_ && (request != askedRequest_ || peer != opponent_ || status != "checking")) {
            asked_ = false;
            if (!HasBasis()) askAtMs_ = nowMs + RetryMs;
        }
        return counts;
    }
    // A game just played: its ping over the whole game says more about the
    // link than the five second check did, so Auto follows it, up and down,
    // until a check measures again. The ping does not depend on the delay
    // played, so following it cannot oscillate. A fighter on a chosen delay, a
    // game that reported too few pings, or one against anyone but the opponent
    // seated now leaves the recommendation as it was. Returns whether it was set.
    bool Played(bool automatic, const PlayedGame& game) {
        if (!automatic || game.opponent.empty() || game.opponent != opponent_ || game.ping.samples < GamePing::MinimumSamples)
            return false;
        basis_ = AutoDelayBasis{AutoDelayBasis::Source::Game, RoundTrip{game.ping.medianMs, game.ping.p95Ms}};
        return true;
    }
    // Auto asked for a check under this request number.
    void Asked(std::uint64_t request) { asked_ = true; askedRequest_ = request; ++attempts_; }

    bool Measured(const std::string& opponent) const { return HasBasis() && !opponent.empty() && opponent == opponent_; }
    // What Auto stands on against `opponent` (None when it is not measured),
    // and the delay that recommends in the game's bands (-1 for none).
    AutoDelayBasis Basis(const std::string& opponent) const { return Measured(opponent) ? basis_ : Unmeasured(); }
    int Recommended(const std::string& opponent) const {
        return Measured(opponent) ? RecommendedInputDelay(bands_, basis_.rtt) : -1;
    }
    // The check is still to be asked for.
    bool Wanted(std::uint64_t nowMs) const {
        return present_ && !HasBasis() && !asked_ && attempts_ < Attempts &&
            nowMs < askAtMs_ + GiveUpMs && nowMs < holdUntilMs_;
    }
    bool Due(std::uint64_t nowMs) const { return Wanted(nowMs) && nowMs >= askAtMs_ && nowMs >= settledFromMs_ + SettleMs; }
    // Ready holds: a check is under way or still to come, and the hold has
    // not run out.
    bool Holding(std::uint64_t nowMs) const {
        return present_ && !HasBasis() && nowMs < holdUntilMs_ && (asked_ || Wanted(nowMs));
    }
    // The delay Auto readies with against `opponent`, in the player's style.
    int Delay(const std::string& opponent, AutoDelayStyle style = AutoDelayStyle::Balanced) const {
        return AutoInputDelay(Recommended(opponent), style);
    }

private:
    static AutoDelayBasis Unmeasured() { return AutoDelayBasis{AutoDelayBasis::Source::None, RoundTrip{0, 0}}; }
    bool HasBasis() const { return basis_.source != AutoDelayBasis::Source::None; }
    void Restart(std::uint64_t nowMs) {
        attempts_ = 0;
        askAtMs_ = nowMs + SettleMs;
        holdUntilMs_ = nowMs + HoldMs;
    }

    InputDelayBands bands_;
    std::string opponent_;
    bool present_ = false;
    AutoDelayBasis basis_ = Unmeasured();
    std::uint64_t firstRequest_ = 1;
    // The newest check result taken, by request number (they only grow).
    std::uint64_t checkedRequest_ = 0;
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
