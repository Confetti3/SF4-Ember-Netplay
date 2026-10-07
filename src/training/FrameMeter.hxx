#pragma once
#include <array>
#include <cstdint>
#include <deque>

namespace sf4e { namespace training {
enum class Phase { Neutral, Movement, Attack, Guard, Hit, Down, Unknown };
enum class BoundaryProvenance : std::uint8_t { None, BacActionHeader };
enum class MeasurementUnavailable : std::uint8_t {
    None, WaitingForAttackBoundary, NoAttackBoundary, NoContact, MeasuringRecovery,
    Interrupted, InvalidSample
};
inline const char* MeasurementUnavailableName(MeasurementUnavailable reason) {
    switch (reason) {
    case MeasurementUnavailable::WaitingForAttackBoundary: return "waiting for the authored attack boundary";
    case MeasurementUnavailable::NoAttackBoundary: return "this native action has no usable attack boundary";
    case MeasurementUnavailable::NoContact: return "waiting for a hit or block";
    case MeasurementUnavailable::MeasuringRecovery: return "measuring recovery";
    case MeasurementUnavailable::Interrupted: return "the exchange was interrupted";
    case MeasurementUnavailable::InvalidSample: return "native action data is unavailable";
    default: return "available";
    }
}
inline Phase ClassifyStatus(unsigned status) {
    // Values are Dimps::Game::Battle::Chara::Actor::Status. Attack is kept
    // whole: AS_SKILL does not distinguish startup, active and recovery.
    switch (status) {
    case 0: case 1: return Phase::Neutral;
    case 2: case 3: case 4: case 5: case 6: case 7: case 8:
    case 9: case 10: case 11: case 12: case 13: return Phase::Movement;
    case 16: return Phase::Attack;
    case 14: case 15: case 22: return Phase::Guard;
    case 17: case 21: case 23: return Phase::Hit;
    case 18: case 19: case 20: return Phase::Down;
    default: return Phase::Unknown;
    }
}
inline const char* PhaseName(Phase phase) {
    switch (phase) {
    case Phase::Neutral: return "Neutral";
    case Phase::Movement: return "Movement";
    case Phase::Attack: return "Attack";
    case Phase::Guard: return "Guard";
    case Phase::Hit: return "Hit / stun";
    case Phase::Down: return "Knockdown";
    default: return "Unknown";
    }
}
struct FighterSample {
    unsigned status = ~0u;
    int action = -1;
    float actionFrame = 0;
    float damage = 0, comboDamage = 0, health = 0;
    bool valid = false;
    // Native action posture: 0 standing, 1 crouching; other values include
    // airborne/downed actions. Unit time scale includes native freezes.
    int posture = -1;
    float timeScale = 0;
    bool basicActionInhibited = true;
    int firstActiveFrame = -1;
    int lastActiveFrame = -1;
    // Native BAC header: the frame the action can be interrupted from, and
    // its total frames; -1 unknown.
    int interruptibleFrame = -1, totalFrames = -1;
    BoundaryProvenance boundaryProvenance = BoundaryProvenance::None;
    // Set by the meter, not the game: the thrower, still in a throw's
    // sequence after the fighter it threw has left it.
    bool throwRecovery = false;
    // Also the meter's: the first frame up from a knockdown, the one a meaty attack is active on.
    bool wake = false;
};
// What a meter cell shows. An attack is told apart by the script's attack
// boundary: before it startup, inside it active, after it recovery; Attack
// alone when the script gives none.
// ponytail: one active stretch per action; a multi-hit move's gaps between
// hits read as active. Read every hit box if they have to show.
// Sequence is AS_SEQUENCE: both fighters while a throw or a cinematic plays;
// what the thrower has left of it once the other is let go is its recovery.
// Down is bouncing or lying, Rise getting up. Meaty is the bars' own, never
// ClassifyMeter's: the frame a fighter is first up, and an attack active on it.
enum class MeterKind { Neutral, Movement, Startup, Active, Recovery, Attack, Guard, Hit, Down, Rise, Sequence, Meaty, Unknown };
inline MeterKind ClassifyMeter(const FighterSample& sample) {
    if (!sample.valid) return MeterKind::Unknown;
    if (sample.status == 24) return sample.throwRecovery ? MeterKind::Recovery : MeterKind::Sequence;
    switch (ClassifyStatus(sample.status)) {
    case Phase::Neutral: return MeterKind::Neutral;
    case Phase::Movement: return MeterKind::Movement;
    case Phase::Guard: return MeterKind::Guard;
    case Phase::Hit: return MeterKind::Hit;
    case Phase::Down: return sample.status == 20 ? MeterKind::Rise : MeterKind::Down;
    case Phase::Attack:
        if (sample.firstActiveFrame < 0 || sample.lastActiveFrame <= sample.firstActiveFrame) return MeterKind::Attack;
        return sample.actionFrame < sample.firstActiveFrame ? MeterKind::Startup :
            sample.actionFrame < sample.lastActiveFrame ? MeterKind::Active : MeterKind::Recovery;
    default: return MeterKind::Unknown;
    }
}
inline bool GroundedRecoveryState(unsigned status) {
    switch (status) {
    case 0: case 1: case 3: case 4: case 7: case 8: case 9:
    case 10: case 11: case 14: case 15: return true;
    default: return false;
    }
}
struct FrameAdvantage {
    std::array<int, 2> frames{};
    bool valid = false, pending = false;
    bool knockdown = false;
    // Who landed the attack this exchange is measured from (-1 before any
    // contact), and whether it was blocked.
    int attacker = -1;
    bool blocked = false;
    MeasurementUnavailable unavailable = MeasurementUnavailable::NoContact;
};
// A fighter's last attack in advancing frames, hitstop left out: those inside
// its attack boundary and those after it. Its startup is MeterView's
// startupFrames, the frame the attack first hits on, so the move's total is
// startup - 1 + active + recovery. seen: an attack was made; live: it is
// still going.
struct MoveFrames { int active = 0, recovery = 0; bool seen = false, live = false; };
// The bars show MeterShown frames; the meter keeps MeterHistory of the
// exchange, twenty seconds, so a held one can be looked back through.
constexpr std::size_t MeterShown = 120, MeterHistory = 1200;
struct MeterFrame {
    std::array<FighterSample, 2> fighters;
    int frame = 0;
};
struct MeterView {
    std::deque<MeterFrame> frames;
    std::array<FighterSample, 2> current;
    std::array<unsigned, 2> stateFrames{};
    std::array<unsigned, 2> actionFrames{};
    std::array<unsigned, 2> lastAttackFrames{};
    std::array<MoveFrames, 2> moves;
    FrameAdvantage advantage;
    std::array<int, 2> startupFrames{{-1, -1}};
    std::array<MeasurementUnavailable, 2> startupUnavailable{{MeasurementUnavailable::NoAttackBoundary,
        MeasurementUnavailable::NoAttackBoundary}};
    std::array<BoundaryProvenance, 2> startupBoundaryProvenance{};
    // Meaty timing: the frame this fighter's attack first became active,
    // counted from the first frame the other was up from a knockdown. 0 meets
    // that frame, -N came N frames early (meaty while N is less than the
    // attack's active frames), +N left the other N frames to act in.
    std::array<int, 2> meatyFrames{};
    std::array<bool, 2> meatyValid{};
    bool frozen = false, autoFreeze = true;
};
class FrameMeter {
public:
    const MeterView& View() const { return view_; }
    void Reset() { const bool autoFreeze = view_.autoFreeze; view_ = MeterView{}; view_.autoFreeze = autoFreeze; idleFrames_ = 0; hadActivity_ = false; hasFrame_ = false; observedFrames_ = contactFrame_ = 0; armed_ = {}; recovered_ = {{-1, -1}}; startupElapsed_ = {}; startupPending_ = {}; thrower_ = {}; firstActiveAt_ = wakeAt_ = {{-1, -1}}; }
    void SetAutoFreeze(bool enabled) { view_.autoFreeze = enabled; view_.frozen = false; }
    void Observe(int frame, const std::array<FighterSample, 2>& observed) {
        // The native fixed-point integral is a wrapping 16-bit counter. It
        // becomes negative after 32767; those values must never double as
        // the "not recovered" sentinel. Check continuity modulo 16 bits and
        // time recovery on an independent clock of accepted observations.
        const auto nativeFrame = static_cast<std::uint16_t>(frame);
        if (hasFrame_ && static_cast<std::uint16_t>(nativeFrame - lastFrame_) != 1) Reset();
        lastFrame_ = nativeFrame; hasFrame_ = true;
        auto fighters = observed;
        // Whoever came into a sequence out of an attack is the one throwing.
        for (int side = 0; side < 2; ++side) {
            const auto& previous = view_.current[side];
            if (!fighters[side].valid || fighters[side].status != 24) thrower_[side] = false;
            else if (!previous.valid || previous.status != 24) thrower_[side] = previous.valid && previous.status == 16;
        }
        for (int side = 0; side < 2; ++side)
            fighters[side].throwRecovery = thrower_[side] && fighters[1 - side].valid && fighters[1 - side].status != 24;
        for (int side = 0; side < 2; ++side)
            fighters[side].wake = fighters[side].valid && view_.current[side].valid && view_.current[side].status == 20 && fighters[side].status != 20;
        const std::int64_t now = observedFrames_++;
        ObserveAdvantage(now, fighters);
        bool neutral = true;
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            neutral = neutral && sample.valid && ClassifyStatus(sample.status) == Phase::Neutral;
            if (sample.valid && view_.current[side].valid && sample.status == view_.current[side].status)
                ++view_.stateFrames[side];
            else view_.stateFrames[side] = sample.valid ? 1 : 0;
            const auto& previous = view_.current[side];
            const bool actionChanged = sample.action != previous.action || sample.actionFrame < previous.actionFrame;
            if (!sample.valid) {
                view_.startupFrames[side] = -1; startupPending_[side] = false;
                view_.startupUnavailable[side] = MeasurementUnavailable::InvalidSample;
                view_.startupBoundaryProvenance[side] = BoundaryProvenance::None;
            } else if (sample.status == 16) {
                if (previous.valid && (previous.status != 16 ||
                    (actionChanged && sample.firstActiveFrame >= 0 && !startupPending_[side]))) {
                    startupElapsed_[side] = 0; startupPending_[side] = true;
                    view_.startupFrames[side] = -1;
                    view_.startupUnavailable[side] = sample.firstActiveFrame >= 0 ?
                        MeasurementUnavailable::WaitingForAttackBoundary : MeasurementUnavailable::NoAttackBoundary;
                    view_.startupBoundaryProvenance[side] = sample.boundaryProvenance;
                }
                // Count accepted advancing frames, not BAC animation ticks:
                // normals and specials commonly change animation speed.
                // A hit can enable hitstop at the end of this very step; use
                // the observed animation advance instead of the next scale.
                if (startupPending_[side] &&
                    (actionChanged || previous.status != 16 || sample.actionFrame > previous.actionFrame)) {
                    ++startupElapsed_[side];
                    if (sample.firstActiveFrame >= 0 && sample.actionFrame >= sample.firstActiveFrame) {
                        view_.startupFrames[side] = startupElapsed_[side];
                        view_.startupBoundaryProvenance[side] = sample.boundaryProvenance;
                        view_.startupUnavailable[side] = MeasurementUnavailable::None;
                        startupPending_[side] = false;
                        firstActiveAt_[side] = now; Meaty(side);
                    }
                }
            } else startupPending_[side] = false;
            auto& move = view_.moves[side];
            if (sample.valid && sample.status == 16) {
                const auto kind = ClassifyMeter(sample);
                const bool began = !previous.valid || previous.status != 16;
                // A cancel into another attack is a new move; a move's own later scripts are not.
                if (began || (actionChanged && kind == MeterKind::Startup)) move = MoveFrames{};
                move.seen = move.live = true;
                if (began || actionChanged || sample.actionFrame > previous.actionFrame) {
                    if (kind == MeterKind::Active) ++move.active;
                    else if (kind == MeterKind::Recovery) ++move.recovery;
                }
            } else if (sample.valid && sample.status == 24 && thrower_[side] && move.seen) {
                // A throw that connected goes on in the sequence.
                move.live = true;
                if (sample.throwRecovery) ++move.recovery;
            } else move.live = false;
            if (sample.valid && previous.valid) {
                const bool down = ClassifyStatus(sample.status) == Phase::Down;
                // A knockdown starts a new meaty reading; the attack that caused it is not one.
                if (down && ClassifyStatus(previous.status) != Phase::Down) {
                    wakeAt_[side] = firstActiveAt_[1 - side] = -1; view_.meatyValid[1 - side] = false;
                }
                if (sample.wake) { wakeAt_[side] = now; Meaty(1 - side); }
            }
            if (sample.valid && previous.valid && sample.action >= 0 && sample.action == previous.action &&
                sample.actionFrame >= previous.actionFrame) ++view_.actionFrames[side];
            else {
                if (previous.valid && sample.valid && previous.action >= 0 && ClassifyStatus(previous.status) == Phase::Attack)
                    view_.lastAttackFrames[side] = view_.actionFrames[side];
                view_.actionFrames[side] = sample.valid && sample.action >= 0 ? 1 : 0;
            }
            view_.current[side] = sample;
        }
        if (neutral) ++idleFrames_;
        else {
            // After a pause the bars start again from their left edge.
            if (view_.frozen || idleFrames_ >= 30) view_.frames.clear();
            view_.frozen = false; idleFrames_ = 0; hadActivity_ = true;
        }
        if (view_.autoFreeze && hadActivity_ && idleFrames_ >= 30) view_.frozen = true;
        if (!view_.frozen) {
            view_.frames.push_back({fighters, frame});
            if (view_.frames.size() > MeterHistory) view_.frames.pop_front();
        }
    }
private:
    void ObserveAdvantage(std::int64_t frame, const std::array<FighterSample, 2>& fighters) {
        auto clear = [&](MeasurementUnavailable reason) {
            view_.advantage = {}; view_.advantage.unavailable = reason; recovered_ = {{-1, -1}};
        };
        for (const auto& sample : fighters) {
            if (!sample.valid || sample.action < 0 || sample.status > 24 ||
                sample.timeScale < 0 ||
                sample.posture < 0) {
                clear(MeasurementUnavailable::InvalidSample); armed_ = {}; return;
            }
        }
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            const auto& previous = view_.current[side];
            const bool attackStarted = sample.status == 16 && previous.valid &&
                (previous.status != 16 || previous.action != sample.action || sample.actionFrame < previous.actionFrame);
            if (attackStarted) {
                if (!armed_[side] || view_.advantage.valid) {
                    clear(MeasurementUnavailable::NoContact); armed_ = {}; armed_[side] = true;
                }
                // Target combos, special cancels and internal action changes
                // continue the exchange. The new action has not recovered yet.
                recovered_[side] = -1;
                contactFrame_ = frame;
            }
        }
        std::array<bool, 2> contacts{};
        for (int defender = 0; defender < 2; ++defender) {
            const auto& sample = fighters[defender];
            const auto& previous = view_.current[defender];
            contacts[defender] = (sample.status == 21 || sample.status == 22 || sample.status == 23) && previous.valid &&
                (sample.status != previous.status || sample.action != previous.action ||
                 sample.actionFrame < previous.actionFrame || sample.comboDamage > previous.comboDamage);
            // A throw connects as its sequence takes both; only the thrown fighter is hit by it.
            if (sample.status == 24 && previous.valid && previous.status != 24 && armed_[1 - defender]) contacts[defender] = true;
        }
        // A trade/interruption cannot inherit the earlier attack's recovery.
        if ((contacts[0] && contacts[1]) || (contacts[0] && armed_[0]) || (contacts[1] && armed_[1])) {
            clear(MeasurementUnavailable::Interrupted); armed_ = {}; return;
        }
        for (int defender = 0; defender < 2; ++defender) {
            if (contacts[defender] && armed_[1 - defender]) {
                view_.advantage.valid = false; view_.advantage.pending = true;
                view_.advantage.unavailable = MeasurementUnavailable::MeasuringRecovery;
                view_.advantage.attacker = 1 - defender;
                view_.advantage.blocked = ClassifyStatus(fighters[defender].status) == Phase::Guard;
                recovered_[defender] = -1;
                // Preserve an already recovered attacker's timestamp: a
                // projectile may connect after its owner's recovery ends.
                contactFrame_ = frame;
            }
        }
        if (frame - contactFrame_ > 600) {
            if (!view_.advantage.valid) clear(MeasurementUnavailable::Interrupted);
            armed_ = {}; return;
        }
        for (int side = 0; side < 2; ++side) {
            const auto& sample = fighters[side];
            if (view_.advantage.pending && (ClassifyStatus(sample.status) == Phase::Down || sample.status == 23))
                view_.advantage.knockdown = true;
            if ((armed_[side] || view_.advantage.pending) && recovered_[side] < 0 &&
                GroundedRecoveryState(sample.status) && sample.posture <= 1 && sample.timeScale > 0 && !sample.basicActionInhibited)
                recovered_[side] = frame;
        }
        if (view_.advantage.pending && recovered_[0] >= 0 && recovered_[1] >= 0) {
            // Positive means this fighter recovered first. Both values are
            // latched together, so the HUD never mixes different exchanges.
            view_.advantage.frames[0] = static_cast<int>(recovered_[1] - recovered_[0]);
            view_.advantage.frames[1] = -view_.advantage.frames[0];
            view_.advantage.valid = true; view_.advantage.pending = false;
            view_.advantage.unavailable = MeasurementUnavailable::None;
        }
    }
    void Meaty(int attacker) {
        const auto active = firstActiveAt_[attacker], wake = wakeAt_[1 - attacker];
        if (active < 0 || wake < 0 || active - wake > 60) return;
        view_.meatyFrames[attacker] = static_cast<int>(active - wake); view_.meatyValid[attacker] = true;
    }
    MeterView view_;
    std::array<bool, 2> thrower_{};
    // On the clock of accepted observations; -1 none.
    std::array<std::int64_t, 2> firstActiveAt_{{-1, -1}}, wakeAt_{{-1, -1}};
    std::array<bool, 2> armed_{};
    std::array<bool, 2> startupPending_{};
    std::array<int, 2> startupElapsed_{};
    std::array<std::int64_t, 2> recovered_{{-1, -1}};
    std::int64_t observedFrames_ = 0, contactFrame_ = 0;
    unsigned idleFrames_ = 0;
    bool hadActivity_ = false;
    std::uint16_t lastFrame_ = 0;
    bool hasFrame_ = false;
};
} }
